/*
 * Stellarium: Meteor Showers Plug-in
 * Copyright (C) 2013-2015 Marcos Cardinot
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA  02110-1335, USA.
 */

#include <QtMath>

#include "LandscapeMgr.hpp"
#include "MeteorShower.hpp"
#include "MeteorShowers.hpp"
#include "SporadicMeteorMgr.hpp"
#include "StelApp.hpp"
#include "StelCore.hpp"
#include "StelModuleMgr.hpp"
#include "StelObjectMgr.hpp"
#include "StelTexture.hpp"
#include "StelUtils.hpp"

MeteorShower::MeteorShower(MeteorShowersMgr* mgr, const QVariantMap& map)
	: m_mgr(mgr)
	, m_status(INVALID)
	, m_speed(0)
	, m_rAlphaPeak(0)
	, m_rDeltaPeak(0)
	, m_driftAlpha(0)
	, m_driftDelta(0)
	, m_pidx(0)
	, m_radiantAlpha(0)
	, m_radiantDelta(0)
{
	// return initialized if the mandatory fields are not present
	if(!map.contains("showerID") || !map.contains("activity")
		|| !map.contains("radiantAlpha") || !map.contains("radiantDelta"))
	{
		qWarning() << "MeteorShower: INVALID meteor shower!" << map.value("showerID").toString();
		qWarning() << "MeteorShower: Please, check your 'showers.json' catalog!";
		return;
	}

	m_showerID = map.value("showerID").toString();
	m_designation  = map.value("designation").toString();
	m_speed = map.value("speed").toInt();
	m_radiantAlpha = StelUtils::getDecAngle(map.value("radiantAlpha").toString());
	m_radiantDelta = StelUtils::getDecAngle(map.value("radiantDelta").toString());
	m_parentObj = map.value("parentObj").toString();
	m_pidx = map.value("pidx").toFloat();

	// the catalog (IMO) will give us the drift for a five-day interval from peak
	m_driftAlpha = StelUtils::getDecAngle(map.value("driftAlpha").toString()) / 5.f;
	m_driftDelta = StelUtils::getDecAngle(map.value("driftDelta").toString()) / 5.f;

	m_rAlphaPeak = m_radiantAlpha;
	m_rDeltaPeak = m_radiantDelta;

	int genericYear = 1000;

	// build the activity list
	QList<QVariant> activities = map.value("activity").toList();
	foreach(const QVariant &ms, activities)
	{
		QVariantMap activityMap = ms.toMap();
		Activity d;
		d.zhr = activityMap.value("zhr").toInt();

		//
		// 'variable'field
		//
		QStringList variable = activityMap.value("variable").toString().split("-");
		if (d.zhr == -1) // is variable
		{
			bool ok = variable.size() == 2;
			for (int i=0; i < 2 && ok; i++)
			{
				d.variable.append(variable.at(i).toInt(&ok));
			}

			if (!ok)
			{
				qWarning() << "MeteorShower: INVALID data for " << m_showerID;
				qWarning() << "MeteorShower: Please, check your 'showers.json' catalog!";
				return;
			}
		}

		//
		// 'start', 'finish' and 'peak' fields
		//
		d.year = activityMap.value("year").toInt();
		QString year = QString::number(d.year == 0 ? genericYear : d.year);

		QString start = activityMap.value("start").toString();
		start = start.isEmpty() ? "" : start + " " + year;
		d.start = QDate::fromString(start, "MM.dd yyyy");

		QString finish = activityMap.value("finish").toString();
		finish = finish.isEmpty() ? "" : finish + " " + year;
		d.finish = QDate::fromString(finish, "MM.dd yyyy");

		QString peak = activityMap.value("peak").toString();
		peak = peak.isEmpty() ? "" : peak + " " + year;
		d.peak = QDate::fromString(peak, "MM.dd yyyy");

		if (d.start.isValid() && d.finish.isValid() && d.peak.isValid())
		{
			// Fix the 'finish' year! Handling cases when the shower starts on
			// the current year and ends on the next year!
			if(d.start.operator >(d.finish))
			{
				d.finish = d.finish.addYears(1);
			}
			// Fix the 'peak' year
			if(d.start.operator >(d.peak))
			{
				d.peak = d.peak.addYears(1);
			}
		}

		m_activities.append(d);
	}

	// filling null values of the activity list with generic data
	const Activity& g = m_activities.at(0);
	const int activitiesSize = m_activities.size();
	for (int i = 1; i < activitiesSize; ++i)
	{
		Activity a = m_activities.at(i);
		if (a.zhr == 0)
		{
			a.zhr = g.zhr;
			a.variable = g.variable;
		}

		int aux = a.year - genericYear;
		a.start = a.start.isValid() ? a.start : g.start.addYears(aux);
		a.finish = a.finish.isValid() ? a.finish : g.finish.addYears(aux);
		a.peak = a.peak.isValid() ? a.peak : g.peak.addYears(aux);
		m_activities.replace(i, a);

		if (!a.start.isValid() || !a.finish.isValid() || !a.peak.isValid())
		{
			qWarning() << "MeteorShower: INVALID data for "
				   << m_showerID << "Unable to read some dates!";
			qWarning() << "MeteorShower: Please, check your 'showers.json' catalog!";
			return;
		}
	}

	if(map.contains("colors"))
	{
		int totalIntensity = 0;
		foreach(const QVariant &ms, map.value("colors").toList())
		{
			QVariantMap colorMap = ms.toMap();
			QString color = colorMap.value("color").toString();
			int intensity = colorMap.value("intensity").toInt();
			m_colors.append(Meteor::ColorPair(color, intensity));
			totalIntensity += intensity;
		}

		// the total intensity must be 100
		if (totalIntensity != 100) {
			qWarning() << "MeteorShower: INVALID data for "
				   << m_showerID << "The total intensity must be equal to 100";
			qWarning() << "MeteorShower: Please, check your 'showers.json' catalog!";
			m_colors.clear();
		}
	}

	if (m_colors.isEmpty()) {
		m_colors.push_back(Meteor::ColorPair("white", 100));
	}

	m_status = UNDEFINED;

	qsrand(QDateTime::currentMSecsSinceEpoch());
}

MeteorShower::~MeteorShower()
{
	qDeleteAll(m_activeMeteors);
	m_activeMeteors.clear();
}

bool MeteorShower::enabled() const
{
	if (m_status == INVALID)
	{
		return false;
	}
	else if (m_status == UNDEFINED)
	{
		return true;
	}
	else if (m_mgr->getActiveRadiantOnly())
	{
		return m_status == ACTIVE_GENERIC || m_status == ACTIVE_CONFIRMED;
	}
	else
	{
		return true;
	}
}

void MeteorShower::update(StelCore* core, double deltaTime)
{
	if (m_status == INVALID)
	{
		return;
	}

	// gets the current UTC date
	double currentJD = core->getJD();
	QDate currentDate = QDate::fromJulianDay(currentJD);

	// updating status and activity
	bool found = false;
	m_status = INACTIVE;
	m_activity = hasConfirmedShower(currentDate, found);
	if (found)
	{
		m_status = ACTIVE_CONFIRMED;
	}
	else
	{
		m_activity = hasGenericShower(currentDate, found);
		if (found)
		{
			m_status = ACTIVE_GENERIC;
		}
	}

	// will be displayed?
	if (!enabled())
	{
		return;
	}

	// fix the radiant position (considering drift)
	m_radiantAlpha = m_rAlphaPeak;
	m_radiantDelta = m_rDeltaPeak;
	if (m_status != INACTIVE)
	{
		double daysToPeak = currentJD - m_activity.peak.toJulianDay();
		m_radiantAlpha += m_driftAlpha * daysToPeak;
		m_radiantDelta += m_driftDelta * daysToPeak;
	}

	// step through and update all active meteors
	foreach (MeteorObj* m, m_activeMeteors)
	{
		if (!m->update(deltaTime))
		{
			m_activeMeteors.removeOne(m);
		}
	}

	// going forward or backward ?
	// don't create new meteors
	if(!core->getRealTimeSpeed())
	{
		return;
	}

	// calculates a ZHR for the current date
	int currentZHR = calculateZHR(currentJD);
	if (currentZHR < 1)
	{
		return;
	}

	// average meteors per frame
	float mpf = currentZHR * deltaTime / 3600.f;

	// maximum amount of meteors for the current frame
	int maxMpf = qRound(mpf);
	maxMpf = maxMpf < 1 ? 1 : maxMpf;

	float rate = mpf / (float) maxMpf;
	for (int i = 0; i < maxMpf; ++i)
	{
		float prob = (float) qrand() / (float) RAND_MAX;
		if (prob < rate)
		{
			MeteorObj *m = new MeteorObj(core, m_speed, m_radiantAlpha, m_radiantDelta,
						     m_pidx, m_colors, m_mgr->getBolideTexture());
			if (m->isAlive())
			{
				m_activeMeteors.append(m);
			}
		}
	}
}

void MeteorShower::draw(StelCore* core)
{
	if (!enabled())
	{
		return;
	}
	drawRadiant(core);
	drawMeteors(core);
}

void MeteorShower::drawRadiant(StelCore *core)
{
	StelPainter painter(core->getProjection(StelCore::FrameJ2000));

	Vec3d XY;
	StelUtils::spheToRect(m_radiantAlpha, m_radiantDelta, m_position);
	painter.getProjector()->project(m_position, XY);

	glEnable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);

	Vec3f rgb;
	float alpha = 0.85f + ((float) qrand() / (float) RAND_MAX) / 10.f;
	switch(m_status)
	{
		case ACTIVE_CONFIRMED: //Active, confirmed data
			rgb = m_mgr->getColorARC();
			break;
		case ACTIVE_GENERIC: //Active, generic data
			rgb = m_mgr->getColorARG();
			break;
		default: //Inactive
			rgb = m_mgr->getColorIR();
	}
	rgb /= 255.f;
	painter.setColor(rgb[0], rgb[1], rgb[2], alpha);

	Vec3d win;
	if (m_mgr->getEnableMarker() && painter.getProjector()->projectCheck(m_position, win))
	{
		m_mgr->getRadiantTexture()->bind();
		painter.drawSprite2dMode(XY[0], XY[1], 45);

		if (m_mgr->getEnableLabels())
		{
			painter.setFont(m_mgr->getFont());
			float size = getAngularSize(NULL)*M_PI/180.*painter.getProjector()->getPixelPerRadAtCenter();
			float shift = 8.f + size/1.8f;
			painter.drawText(XY[0]+shift, XY[1]+shift, getNameI18n(), 0, 0, 0, false);
		}
	}
}

void MeteorShower::drawMeteors(StelCore *core)
{
	if (!core->getSkyDrawer()->getFlagHasAtmosphere())
	{
		return;
	}

	LandscapeMgr* landmgr = GETSTELMODULE(LandscapeMgr);
	if (landmgr->getFlagAtmosphere() && landmgr->getLuminance() > 5.f)
	{
		return;
	}

	// step through and draw all active meteors
	StelPainter painter(core->getProjection(StelCore::FrameAltAz));
	foreach (MeteorObj* m, m_activeMeteors)
	{
		m->draw(core, painter);
	}
}

MeteorShower::Activity MeteorShower::hasGenericShower(QDate date, bool &found) const
{
	int year = date.year();
	Activity g = m_activities.at(0);
	bool peakOnStart = g.peak.year() == g.start.year(); // 'peak' and 'start' on the same year ?

	//Fix the 'generic years'!
	// Handling cases when the shower starts on the current year and
	// ends on the next year; or when it started on the last year...
	if (g.start.year() != g.finish.year()) // edge case?
	{
		// trying the current year with the next year
		g.start.setDate(year, g.start.month(), g.start.day());
		g.finish.setDate(year + 1, g.finish.month(), g.finish.day());
		found = date.operator >=(g.start) && date.operator <=(g.finish);

		if (!found)
		{
			// trying the last year with the current year
			g.start.setDate(year - 1, g.start.month(), g.start.day());
			g.finish.setDate(year, g.finish.month(), g.finish.day());
			found = date.operator >=(g.start) && date.operator <=(g.finish);
		}
	}
	else
	{
		g.start.setDate(year, g.start.month(), g.start.day());
		g.finish.setDate(year, g.finish.month(), g.finish.day());
		found = date.operator >=(g.start) && date.operator <=(g.finish);
	}

	if (found)
	{
		g.year = g.start.year();
		g.peak.setDate(peakOnStart ? g.start.year() : g.finish.year(),
			       g.peak.month(),
			       g.peak.day());
		return g;
	}
	return Activity();
}

MeteorShower::Activity MeteorShower::hasConfirmedShower(QDate date, bool& found) const
{
	const int activitiesSize = m_activities.size();
	for (int i = 1; i < activitiesSize; ++i)
	{
		const Activity& a = m_activities.at(i);
		if (date.operator >=(a.start) && date.operator <=(a.finish))
		{
			found = true;
			return a;
		}
	}
	return Activity();
}

int MeteorShower::calculateZHR(const double& currentJD)
{
	double startJD = m_activity.start.toJulianDay();
	double finishJD = m_activity.finish.toJulianDay();
	double peakJD = m_activity.peak.toJulianDay();

	float sd; //standard deviation
	if (currentJD >= startJD && currentJD < peakJD) //left side of gaussian
	{
		sd = (peakJD - startJD) / 2.f;
	}
	else
	{
		sd = (finishJD - peakJD) / 2.f;
	}

	float maxZHR = m_activity.zhr == -1 ? m_activity.variable.at(1) : m_activity.zhr;
	float minZHR = m_activity.zhr == -1 ? m_activity.variable.at(0) : 0;

	float gaussian = maxZHR * qExp( - qPow(currentJD - peakJD, 2) / (sd * sd) ) + minZHR;

	return qRound(gaussian);
}

QString MeteorShower::getSolarLongitude(QDate date) const
{
	//The number of days (positive or negative) since Greenwich noon,
	//Terrestrial Time, on 1 January 2000 (J2000.0)
	double n = date.toJulianDay() - 2451545.0;

	//The mean longitude of the Sun, corrected for the aberration of light
	float l = 280.460 + 0.9856474 * n;

	// put it in the range 0 to 360 degrees
	l /= 360.f;
	l = (l - (int) l) * 360.f - 1.f;

	return QString::number(l, 'f', 2);
}

QString MeteorShower::getDesignation() const
{
	if (m_showerID.toInt()) // if showerID is a number
	{
		return "";
	}
	return m_showerID;
}

Vec3f MeteorShower::getInfoColor(void) const
{
	return StelApp::getInstance().getVisionModeNight() ? Vec3f(0.6, 0.0, 0.0) : Vec3f(1.0, 1.0, 1.0);
}

QString MeteorShower::getInfoString(const StelCore* core, const InfoStringGroup& flags) const
{
	if (!enabled())
	{
		GETSTELMODULE(StelObjectMgr)->unSelect();
		return "";
	}

	QString str;
	QTextStream oss(&str);

	QString mstdata;
	if (m_status == ACTIVE_GENERIC)
	{
		mstdata = q_("generic data");
	}
	else if (m_status == ACTIVE_CONFIRMED)
	{
		mstdata = q_("confirmed data");
	}
	else if (m_status == INACTIVE)
	{
		mstdata = q_("inactive");
	}

	if(flags&Name)
	{
		oss << "<h2>" << getNameI18n();
		if (!m_showerID.toInt())
		{
			oss << " (" << m_showerID  <<")</h2>";
		}
		else
		{
			oss << "</h2>";
		}
	}

	if(flags&Extra)
	{
		oss << q_("Type: <b>%1</b> (%2)").arg(q_("meteor shower"), mstdata) << "<br />";
	}

	// Ra/Dec etc.
	oss << getPositionInfoString(core, flags);

	if(flags&Extra)
	{
		oss << QString("%1: %2/%3")
			.arg(q_("Radiant drift (per day)"))
			.arg(StelUtils::radToHmsStr(m_driftAlpha))
			.arg(StelUtils::radToDmsStr(m_driftDelta));
		oss << "<br />";

		if (m_speed > 0)
		{
			oss << q_("Geocentric meteoric velocity: %1 km/s").arg(m_speed) << "<br />";
		}

		if(m_pidx > 0)
		{
			oss << q_("The population index: %1").arg(m_pidx) << "<br />";
		}

		if(!m_parentObj.isEmpty())
		{
			oss << q_("Parent body: %1").arg(q_(m_parentObj)) << "<br />";
		}

		// activity info
		if (m_status != INACTIVE)
		{
			if(m_activity.start.month() == m_activity.finish.month())
			{
				oss << QString("%1: %2 - %3 %4")
				       .arg(q_("Active"))
				       .arg(m_activity.start.day())
				       .arg(m_activity.finish.day())
				       .arg(m_activity.start.toString("MMMM"));
			}
			else
			{
				oss << QString("%1: %2 - %3")
				       .arg(q_("Activity"))
				       .arg(m_activity.start.toString("d MMMM"))
				       .arg(m_activity.finish.toString("d MMMM"));
			}
			oss << "<br />";
			oss << q_("Maximum: %1").arg(m_activity.peak.toString("d MMMM"));

			oss << QString(" (%1 %2&deg;)").arg(q_("Solar longitude"))
			       .arg(getSolarLongitude(m_activity.peak));
			oss << "<br />";

			if(m_activity.zhr > 0)
			{
				oss << QString("ZHR<sub>max</sub>: %1").arg(m_activity.zhr) << "<br />";
			}
			else
			{
				oss << QString("ZHR<sub>max</sub>: %1").arg(q_("variable"));
				if(m_activity.variable.size() == 2)
				{
					oss << QString("; %1-%2").arg(m_activity.variable.at(0))
					       .arg(m_activity.variable.at(1));
				}
				oss << "<br />";
			}
		}
	}

	postProcessInfoString(str, flags);

	return str;
}
