/*
 * Copyright (c) 2012 Red Hat.
 * Copyright (c) 2012 Nathan Scott.  All Rights Reserved.
 * Copyright (c) 2006-2010, Aconex.  All Rights Reserved.
 * Copyright (c) 2006, Ken McDonell.  All Rights Reserved.
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */
#include <qmc_desc.h>
#include "main.h"
#include "tracing.h"
#include "sampling.h"
#include "saveviewdialog.h"

#include <QtCore/QPoint>
#include <QtCore/QRegExp>
#include <QtGui/QApplication>
#include <QtGui/QPainter>
#include <QtGui/QLabel>
#include <qwt_plot_layout.h>
#include <qwt_plot_canvas.h>
#include <qwt_plot_curve.h>
#include <qwt_plot_picker.h>
#include <qwt_double_rect.h>
#include <qwt_legend.h>
#include <qwt_legend_item.h>
#include <qwt_scale_widget.h>
#include <qwt_text.h>
#include <qwt_text_label.h>

#define DESPERATE 0

ChartItem::ChartItem(QmcMetric *mp, pmMetricSpec *msp, pmDesc *dp,
			Chart::Style style, const char *legend)
{
    my.metric = mp;
    my.units = dp->units;

    my.name = QString(msp->metric);
    if (msp->ninst == 1)
	my.name.append("[").append(msp->inst[0]).append("]");

    //
    // Build the legend label string, even if the chart is declared
    // "legend off" so that subsequent Edit->Chart Title and Legend
    // changes can turn the legend on and off dynamically
    //
    if (legend != NULL) {
	my.legend = strdup(legend);
	my.label = QString(legend);
    } else {
	my.legend = NULL;
	// show name as ...[end of name]
	if (my.name.size() > PmChart::maximumLegendLength()) {
	    int size = PmChart::maximumLegendLength() - 3;
	    my.label = QString("...");
	    my.label.append(my.name.right(size));
	} else {
	    my.label = my.name;
	}
    }

    my.removed = false;
    my.hidden = false;

    my.scale = 1;
    if (dp->sem == PM_SEM_COUNTER && dp->units.dimTime == 0 &&
	style != Chart::UtilisationStyle) {
	// value to plot is time / time ... set scale
	if (dp->units.scaleTime == PM_TIME_USEC)
	    my.scale = 0.000001;
	else if (dp->units.scaleTime == PM_TIME_MSEC)
	    my.scale = 0.001;
    }
}

ChartItem::~ChartItem()
{
    if (my.data != NULL)
	free(my.data);
    if (my.itemData != NULL)
	free(my.itemData);
}

Chart::Chart(Tab *chartTab, QWidget *parent) : QwtPlot(parent), Gadget()
{
    Gadget::setWidget(this);
    setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::MinimumExpanding);
    plotLayout()->setCanvasMargin(0);
    plotLayout()->setAlignCanvasToScales(true);
    plotLayout()->setFixedAxisOffset(54, QwtPlot::yLeft);
    setAutoReplot(false);
    setMargin(1);
    setCanvasBackground(globalSettings.chartBackground);
    canvas()->setPaintAttribute(QwtPlotCanvas::PaintPacked, true);
    enableAxis(xBottom, false);

    setLegendVisible(true);
    legend()->contentsWidget()->setFont(*globalFont);
    connect(this, SIGNAL(legendChecked(QwtPlotItem *, bool)),
	    SLOT(showItem(QwtPlotItem *, bool)));

    my.tab = chartTab;
    my.title = NULL;
    my.eventType = false;
    my.rateConvert = true;
    my.antiAliasing = true;
    my.style = NoStyle;
    my.scheme = QString::null;
    my.sequence = 0;
    my.tracingScaleEngine = NULL;
    my.samplingScaleEngine = NULL;
    setAxisFont(QwtPlot::yLeft, *globalFont);
    setAxisAutoScale(QwtPlot::yLeft);
    setScaleEngine();

    my.picker = new QwtPlotPicker(QwtPlot::xBottom, QwtPlot::yLeft,
			QwtPicker::PointSelection | QwtPicker::DragSelection,
			QwtPlotPicker::CrossRubberBand, QwtPicker::AlwaysOff,
			canvas());
    my.picker->setRubberBandPen(QColor(Qt::green));
    my.picker->setRubberBand(QwtPicker::CrossRubberBand);
    my.picker->setTrackerPen(QColor(Qt::white));
    connect(my.picker, SIGNAL(selected(const QwtDoublePoint &)),
			 SLOT(selected(const QwtDoublePoint &)));
    connect(my.picker, SIGNAL(moved(const QwtDoublePoint &)),
			 SLOT(moved(const QwtDoublePoint &)));

    replot();

    console->post("Chart::ctor complete(%p)", this);
}

void Chart::setScaleEngine()
{
    if (my.eventType && !my.tracingScaleEngine) {
	my.tracingScaleEngine = new TracingScaleEngine();
	setAxisScaleEngine(QwtPlot::yLeft, my.tracingScaleEngine);
	my.samplingScaleEngine = NULL;	// deleted in setAxisScaleEngine
    } else if (!my.eventType && !my.samplingScaleEngine) {
	my.samplingScaleEngine = new SamplingScaleEngine();
	setAxisScaleEngine(QwtPlot::yLeft, my.samplingScaleEngine);
	my.tracingScaleEngine = NULL;	// deleted in setAxisScaleEngine
    }
}

Chart::~Chart()
{
    console->post("Chart::~Chart() for chart %p", this);

    for (int i = 0; i < my.items.size(); i++)
	delete my.items[i];
    delete my.picker;
}

void Chart::setCurrent(bool enable)
{
    QwtScaleWidget *sp;
    QPalette palette;
    QwtText t;

    console->post("Chart::setCurrent(%p) %s", this, enable ? "true" : "false");

    // (Re)set title and y-axis highlight for new/old current chart.
    // For title, have to set both QwtText and QwtTextLabel because of
    // the way attributes are cached and restored when printing charts

    t = titleLabel()->text();
    t.setColor(enable ? globalSettings.chartHighlight : "black");
    setTitle(t);
    palette = titleLabel()->palette();
    palette.setColor(QPalette::Active, QPalette::Text,
		enable ? globalSettings.chartHighlight : QColor("black"));
    titleLabel()->setPalette(palette);

    sp = axisWidget(QwtPlot::yLeft);
    t = sp->title();
    t.setColor(enable ? globalSettings.chartHighlight : "black");
    sp->setTitle(t);
}

void Chart::preserveLiveData(int index, int oldindex)
{
#if DESPERATE
    console->post("Chart::preserveLiveData %d/%d (%d items)",
			index, oldindex, my.items.size());
#endif

    if (my.items.size() < 1)
	return;
    for (int i = 0; i < my.items.size(); i++)
	my.items[i]->preserveLiveData(index, oldindex);
}

void ChartItem::preserveLiveData(int index, int oldindex)
{
    if (my.dataCount > oldindex)
	my.itemData[index] = my.data[index] = my.data[oldindex];
    else
	my.itemData[index] = my.data[index] = SamplingCurve::NaN();
}

void Chart::punchoutLiveData(int index)
{
#if DESPERATE
    console->post("Chart::punchoutLiveData=%d (%d items)", i, my.items.size());
#endif

    if (my.items.size() < 1)
	return;
    for (int i = 0; i < my.items.size(); i++)
	my.items[i]->punchoutLiveData(index);
}

void ChartItem::punchoutLiveData(int index)
{
    my.data[index] = my.itemData[index] = SamplingCurve::NaN();
}

void Chart::adjustValues()
{
    redoChartItems();
    replot();
}

void Chart::updateTimeAxis(double leftmost, double rightmost, double delta)
{
    setAxisScale(QwtPlot::xBottom, leftmost, rightmost, delta);
}

void Chart::updateValues(bool forward, bool visible)
{
    int		sh = my.tab->group()->sampleHistory();
    int		index, i;

#if DESPERATE
    console->post(PmChart::DebugForce,
		  "Chart::updateValues(forward=%d,visible=%d) sh=%d (%d items)",
		  forward, visible, sh, my.items.size());
#endif

    if (my.items.size() < 1)
	return;

    for (i = 0; i < my.items.size(); i++) {
	ChartItem *item = my.items[i];
	double value;
	int sz;

	if (item->my.metric->error(0)) {
	    value = SamplingCurve::NaN();
	} else {
	    // convert raw value to current chart scale
	    pmAtomValue	raw;
	    pmAtomValue	scaled;
	    raw.d = my.rateConvert ?
			item->my.metric->value(0) : item->my.metric->currentValue(0);
	    pmConvScale(PM_TYPE_DOUBLE, &raw, &item->my.units, &scaled, &my.units);
	    value = scaled.d * item->my.scale;
	}

	if (item->my.dataCount < sh)
	    sz = qMax(0, (int)(item->my.dataCount * sizeof(double)));
	else
	    sz = qMax(0, (int)((item->my.dataCount - 1) * sizeof(double)));

#if DESPERATE
	console->post(PmChart::DebugForce,
		"BEFORE Chart::update (%s) 0-%d (sz=%d,v=%.2f):",
		(const char *)item->my.metric->name().toAscii(),
		item->my.dataCount, sz, value);
	for (index = 0; index < item->my.dataCount; index++)
	    console->post("\t[%d] data=%.2f", index, item->my.data[index]);
#endif

	if (forward) {
	    memmove(&item->my.data[1], &item->my.data[0], sz);
	    memmove(&item->my.itemData[1], &item->my.itemData[0], sz);
	    item->my.data[0] = value;
	} else {
	    memmove(&item->my.data[0], &item->my.data[1], sz);
	    memmove(&item->my.itemData[0], &item->my.itemData[1], sz);
	    item->my.data[item->my.dataCount - 1] = value;
	}
	if (item->my.dataCount < sh)
	    item->my.dataCount++;

#if DESPERATE
	console->post(PmChart::DebugForce, "AFTER Chart::update (%s) 0-%d:",
			(const char *)item->my.name.toAscii(), item->my.dataCount);
	for (index = 0; index < item->my.dataCount; index++)
	    console->post(PmChart::DebugForce, "\t[%d] data=%.2f time=%s",
				index, item->my.data[index],
				timeString(my.tab->timeAxisData()[index]));
#endif
    }

    if (my.style == BarStyle || my.style == AreaStyle || my.style == LineStyle) {
	index = 0;
	for (i = 0; i < my.items.size(); i++) {
	    if (!forward)
		index = my.items[i]->my.dataCount - 1;
	    my.items[i]->my.itemData[index] = my.items[i]->my.data[index];
	}
    }
    else if (my.style == UtilisationStyle) {
	// like Stack, but normalize value to a percentage (0,100)
	double sum = 0.0;
	index = 0;
	// compute sum
	for (i = 0; i < my.items.size(); i++) {
	    if (!forward)
		index = my.items[i]->my.dataCount - 1;
	    if (!SamplingCurve::isNaN(my.items[i]->my.data[index]))
		sum += my.items[i]->my.data[index];
	}
	// scale all components
	for (i = 0; i < my.items.size(); i++) {
	    if (!forward)
		index = my.items[i]->my.dataCount - 1;
	    if (sum == 0 || my.items[i]->my.hidden ||
		SamplingCurve::isNaN(my.items[i]->my.data[index]))
		my.items[i]->my.itemData[index] = SamplingCurve::NaN();
	    else
		my.items[i]->my.itemData[index] = 100 * my.items[i]->my.data[index] / sum;
	}
	// stack components
	sum = 0.0;
	for (i = 0; i < my.items.size(); i++) {
	    if (!forward)
		index = my.items[i]->my.dataCount - 1;
	    if (!SamplingCurve::isNaN(my.items[i]->my.itemData[index])) {
		sum += my.items[i]->my.itemData[index];
		my.items[i]->my.itemData[index] = sum;
	    }
	}
    }
    else if (my.style == StackStyle) {
	double sum = 0.0;
	index = 0;
	for (i = 0; i < my.items.size(); i++) {
	    if (!forward)
		index = my.items[0]->my.dataCount - 1;
	    if (my.items[i]->my.hidden || SamplingCurve::isNaN(my.items[i]->my.data[index])) {
		my.items[i]->my.itemData[index] = SamplingCurve::NaN();
	    } else {
		sum += my.items[i]->my.data[index];
		my.items[i]->my.itemData[index] = sum;
	    }
	}
    }

#if DESPERATE
    for (i = 0; i < my.items.size(); i++)
	console->post(PmChart::DebugForce, "metric[%d] value %f item %f", m,
		my.items[i]->my.metric->value(0), my.rateConvert ?
		my.items[i]->my.metric->value(0) : my.items[i]->my.metric->currentValue(0),
		my.items[i]->itemData[0]);
#endif

    if (visible) {
	// replot() first so Value Axis range is updated
	replot();
	redoScale();
    }
}

bool Chart::autoScale(void)
{
    if (my.eventType)
	return false;
    return my.samplingScaleEngine->autoScale();
}

void Chart::redoScale(void)
{
    bool	rescale = false;
    pmUnits	oldunits = my.units;
    int		m;	// TODO: i

    // The 1,000 and 0.1 thresholds are just a heuristic guess.
    //
    // We're assuming lBound() plays no part in this, which is OK as
    // the upper bound of the y-axis range (hBound()) drives the choice
    // of appropriate units scaling.
    //
    if (autoScale() &&
	axisScaleDiv(QwtPlot::yLeft)->upperBound() > 1000) {
	double scaled_max = axisScaleDiv(QwtPlot::yLeft)->upperBound();
	if (my.units.dimSpace == 1) {
	    switch (my.units.scaleSpace) {
		case PM_SPACE_BYTE:
		    my.units.scaleSpace = PM_SPACE_KBYTE;
		    rescale = true;
		    break;
		case PM_SPACE_KBYTE:
		    my.units.scaleSpace = PM_SPACE_MBYTE;
		    rescale = true;
		    break;
		case PM_SPACE_MBYTE:
		    my.units.scaleSpace = PM_SPACE_GBYTE;
		    rescale = true;
		    break;
		case PM_SPACE_GBYTE:
		    my.units.scaleSpace = PM_SPACE_TBYTE;
		    rescale = true;
		    break;
		case PM_SPACE_TBYTE:
		    my.units.scaleSpace = PM_SPACE_PBYTE;
		    rescale = true;
		    break;
		case PM_SPACE_PBYTE:
		    my.units.scaleSpace = PM_SPACE_EBYTE;
		    rescale = true;
		    break;
	    }
	    if (rescale) {
		// logic here depends on PM_SPACE_* values being consecutive
		// integer values as the scale increases
		scaled_max /= 1024;
		while (scaled_max > 1000) {
		    my.units.scaleSpace++;
		    scaled_max /= 1024;
		    if (my.units.scaleSpace == PM_SPACE_EBYTE) break;
		}
	    }
	}
	else if (my.units.dimTime == 1) {
	    switch (my.units.scaleTime) {
		case PM_TIME_NSEC:
		    my.units.scaleTime = PM_TIME_USEC;
		    rescale = true;
		    scaled_max /= 1000;
		    break;
		case PM_TIME_USEC:
		    my.units.scaleTime = PM_TIME_MSEC;
		    rescale = true;
		    scaled_max /= 1000;
		    break;
		case PM_TIME_MSEC:
		    my.units.scaleTime = PM_TIME_SEC;
		    rescale = true;
		    scaled_max /= 1000;
		    break;
		case PM_TIME_SEC:
		    my.units.scaleTime = PM_TIME_MIN;
		    rescale = true;
		    scaled_max /= 60;
		    break;
		case PM_TIME_MIN:
		    my.units.scaleTime = PM_TIME_HOUR;
		    rescale = true;
		    scaled_max /= 60;
		    break;
	    }
	    if (rescale) {
		// logic here depends on PM_TIME* values being consecutive
		// integer values as the scale increases
		while (scaled_max > 1000) {
		    my.units.scaleTime++;
		    if (my.units.scaleTime <= PM_TIME_SEC)
			scaled_max /= 1000;
		    else
			scaled_max /= 60;
		    if (my.units.scaleTime == PM_TIME_HOUR) break;
		}
	    }
	}
    }

    if (rescale == false && autoScale() &&
	axisScaleDiv(QwtPlot::yLeft)->upperBound() < 0.1) {
	double scaled_max = axisScaleDiv(QwtPlot::yLeft)->upperBound();
	if (my.units.dimSpace == 1) {
	    switch (my.units.scaleSpace) {
		case PM_SPACE_KBYTE:
		    my.units.scaleSpace = PM_SPACE_BYTE;
		    rescale = true;
		    break;
		case PM_SPACE_MBYTE:
		    my.units.scaleSpace = PM_SPACE_KBYTE;
		    rescale = true;
		    break;
		case PM_SPACE_GBYTE:
		    my.units.scaleSpace = PM_SPACE_MBYTE;
		    rescale = true;
		    break;
		case PM_SPACE_TBYTE:
		    my.units.scaleSpace = PM_SPACE_GBYTE;
		    rescale = true;
		    break;
		case PM_SPACE_PBYTE:
		    my.units.scaleSpace = PM_SPACE_TBYTE;
		    rescale = true;
		    break;
		case PM_SPACE_EBYTE:
		    my.units.scaleSpace = PM_SPACE_PBYTE;
		    rescale = true;
		    break;
	    }
	    if (rescale) {
		// logic here depends on PM_SPACE_* values being consecutive
		// integer values (in reverse) as the scale decreases
		scaled_max *= 1024;
		while (scaled_max < 0.1) {
		    my.units.scaleSpace--;
		    scaled_max *= 1024;
		    if (my.units.scaleSpace == PM_SPACE_BYTE) break;
		}
	    }
	}
	else if (my.units.dimTime == 1) {
	    switch (my.units.scaleTime) {
		case PM_TIME_USEC:
		    my.units.scaleTime = PM_TIME_NSEC;
		    rescale = true;
		    scaled_max *= 1000;
		    break;
		case PM_TIME_MSEC:
		    my.units.scaleTime = PM_TIME_USEC;
		    rescale = true;
		    scaled_max *= 1000;
		    break;
		case PM_TIME_SEC:
		    my.units.scaleTime = PM_TIME_MSEC;
		    rescale = true;
		    scaled_max *= 1000;
		    break;
		case PM_TIME_MIN:
		    my.units.scaleTime = PM_TIME_SEC;
		    rescale = true;
		    scaled_max *= 60;
		    break;
		case PM_TIME_HOUR:
		    my.units.scaleTime = PM_TIME_MIN;
		    rescale = true;
		    scaled_max *= 60;
		    break;
	    }
	    if (rescale) {
		// logic here depends on PM_TIME* values being consecutive
		// integer values (in reverse) as the scale decreases
		while (scaled_max < 0.1) {
		    my.units.scaleTime--;
		    if (my.units.scaleTime < PM_TIME_SEC)
			scaled_max *= 1000;
		    else
			scaled_max *= 60;
		    if (my.units.scaleTime == PM_TIME_NSEC) break;
		}
	    }
	}
    }

    if (rescale) {
	pmAtomValue	old_av;
	pmAtomValue	new_av;

	console->post("Chart::update change units %s", pmUnitsStr(&my.units));
	// need to rescale ... we transform all of the historical (raw)
	// data[] and the itemData[] ... new data will be taken care of
	// by changing my.units
	//
	for (m = 0; m < my.items.size(); m++) {
	    for (int index = my.items[m]->my.dataCount-1; index >= 0; index--) {
		if (my.items[m]->my.data[index] != SamplingCurve::NaN()) {
		    old_av.d = my.items[m]->my.data[index];
		    pmConvScale(PM_TYPE_DOUBLE, &old_av, &oldunits, &new_av, &my.units);
		    my.items[m]->my.data[index] = new_av.d;
		}
		if (my.items[m]->my.itemData[index] != SamplingCurve::NaN()) {
		    old_av.d = my.items[m]->my.itemData[index];
		    pmConvScale(PM_TYPE_DOUBLE, &old_av, &oldunits, &new_av, &my.units);
		    my.items[m]->my.itemData[index] = new_av.d;
		}
	    }
	}
	if (my.style == UtilisationStyle) {
	    setYAxisTitle("% utilization");
	} else {
	    setYAxisTitle(pmUnitsStr(&my.units));
	}

	replot();
    }
}

void Chart::replot()
{
    int	vh = my.tab->group()->visibleHistory();

#if DESPERATE
    console->post("Chart::replot vh=%d, %d items)", vh, my.items.size());
#endif

    for (int i = 0; i < my.items.size(); i++)
	my.items[i]->my.curve->setRawData(my.tab->group()->timeAxisData(),
					my.items[i]->my.itemData,
					qMin(vh, my.items[i]->my.dataCount));
    QwtPlot::replot();
}

void Chart::showItem(QwtPlotItem *item, bool on)
{
    item->setVisible(on);
    if (legend()) {
	QWidget *w = legend()->find(item);
	if (w && w->inherits("QwtLegendItem")) {
	    QwtLegendItem *li = (QwtLegendItem *)w;
	    li->setChecked(on);
	    li->setFont(*globalFont);
	}
    }
    // find matching item and update hidden status if required
    for (int i = 0; i < my.items.size(); i++) {
	if (item == my.items[i]->my.curve) {
	    if (my.items[i]->my.hidden == on) {
		// boolean sense is reversed here, on == true => show item
		my.items[i]->my.hidden = !on;
		redoChartItems();
	    }
	    break;
	}
    }
    replot();
}

void Chart::resetValues(ChartItem *item, int values)
{
    size_t size;

    // Reset sizes of pcp data array, the item data array, and the time array
    size = values * sizeof(item->my.data[0]);
    if ((item->my.data = (double *)realloc(item->my.data, size)) == NULL)
	nomem();
    size = values * sizeof(item->my.itemData[0]);
    if ((item->my.itemData = (double *)realloc(item->my.itemData, size)) == NULL)
	nomem();
    if (item->my.dataCount > values)
	item->my.dataCount = values;
}

void Chart::resetValues(int index, int values)
{
    resetValues(my.items[index], values);
}

// add a new plot
// the pmMetricSpec has been filled in, and ninst is always 0
// (PM_INDOM_NULL) or 1 (one instance at a time)
//
int Chart::addItem(pmMetricSpec *pmsp, const char *legend)
{
    int		maxCount;
    QmcMetric	*mp;
    pmDesc	desc;

    console->post("Chart::addItem src=%s", pmsp->source);
    if (pmsp->ninst == 0)
	console->post("addItem metric=%s", pmsp->metric);
    else
	console->post("addItem instance %s[%s]", pmsp->metric, pmsp->inst[0]);

    mp = my.tab->group()->addMetric(pmsp, 0.0, true);
    if (mp->status() < 0)
	return mp->status();
    desc = mp->desc().desc();
    if (my.rateConvert && desc.sem == PM_SEM_COUNTER) {
	if (desc.units.dimTime == 0) {
	    desc.units.dimTime = -1;
	    desc.units.scaleTime = PM_TIME_SEC;
	}
	else if (desc.units.dimTime == 1) {
	    desc.units.dimTime = 0;
	    // don't play with scaleTime, need native per item scaleTime
	    // so we can apply correct scaling via item->scale, e.g. in
	    // the msec -> msec/sec after rate conversion ... see the
	    // calculation for item->scale below
	}
    }

    if (my.items.size() == 0) {
	console->post("Chart::addItem initial units %s", pmUnitsStr(&my.units));
	my.units = desc.units;
	my.eventType = (desc.type == PM_TYPE_EVENT);
	my.style = my.eventType ? EventStyle : my.style;
	setScaleEngine();
    }
    else {
	// error reporting handled in caller
	if (checkCompatibleUnits(&desc.units) == false)
	    return PM_ERR_CONV;
	if (checkCompatibleTypes(desc.type) == false)
	    return PM_ERR_CONV;
    }

    ChartItem *item = new ChartItem(mp, pmsp, &desc, my.style, legend);
    my.items.append(item);
    console->post("addItem item=%p nitems=%d", item, my.items.size());

    // initialize the pcp data and item data arrays
    item->my.dataCount = 0;
    item->my.data = NULL;
    item->my.itemData = NULL;
    resetValues(item, my.tab->group()->sampleHistory());

    // create and attach the plot right here
    item->my.curve = new SamplingCurve(item->my.label);
    item->my.curve->attach(this);

    // the 1000 is arbitrary ... just want numbers to be monotonic
    // decreasing as plots are added
    item->my.curve->setZ(1000 - my.items.size() - 1);

    // force plot to be visible, legend visibility is controlled by
    // legend() to a state matching the initial state
    showItem(item->my.curve, true);

    // set the prevailing chart style and the default color
    setStroke(item, my.style, nextColor(my.scheme, &my.sequence));

    maxCount = 0;
    for (int i = 0; i < my.items.size(); i++)
	maxCount = qMax(maxCount, my.items[i]->my.dataCount);
    // Set all the values for all items from dataCount to maxCount to zero
    // so that the Stack <--> Line transitions work correctly
    for (int i = 0; i < my.items.size(); i++) {
	for (int index = my.items[i]->my.dataCount+1; index < maxCount; index++)
	    my.items[i]->my.data[index] = 0;
	// don't re-set dataCount ... so we don't plot these values,
	// we just want them to count 0 towards any Stack aggregation
    }

    return my.items.size() - 1;
}

void Chart::reviveItem(int i)
{
    console->post("Chart::reviveItem=%d (%d)", i, my.items[i]->my.removed);

    if (my.items[i]->my.removed) {
	my.items[i]->my.removed = false;
	my.items[i]->my.curve->attach(this);
    }
}

void Chart::removeItem(int i)
{
    console->post("Chart::removeItem item=%d", i);

    my.items[i]->my.removed = true;
    my.items[i]->my.curve->detach();

    // We can't really do this properly (free memory, etc) - working around
    // metrics class limit (its using an ordinal index for metrics, remove any
    // and we'll get problems.  Which means the plots array must also remain
    // unchanged, as we drive things via the metriclist at times.  D'oh.
    // This blows - it means we have to continue to fetch metrics for those
    // metrics that have been removed from the chart, which may be remote
    // hosts, hosts which are down (introducing retry issues...).  Bother.

    //delete my.items[i]->curve;
    //delete my.items[i]->label;
    //free(my.items[i]->legend);
    //my.items.removeAt(i);
}

int Chart::metricCount() const
{
    return my.items.size();
}

char *Chart::title()
{
    return my.title;
}

// expand is true to expand %h to host name in title
//
void Chart::changeTitle(char *title, int expand)
{
    bool hadTitle = (my.title != NULL);

    if (my.title) {
	free(my.title);
	my.title = NULL;
    }
    if (title != NULL) {
	if (hadTitle)
	    pmchart->updateHeight(titleLabel()->height());
	QwtText t = titleLabel()->text();
	t.setFont(*globalFont);
	setTitle(t);
	// have to set font for both QwtText and QwtTextLabel because of
	// the way attributes are cached and restored when printing charts
	QFont titleFont = *globalFont;
	titleFont.setBold(true);
	titleLabel()->setFont(titleFont);
	my.title = strdup(title);

	if (expand && (strstr(title, "%h")) != NULL) {
	    QString titleString = title;
	    QString shortHost = activeGroup->context()->source().host();
	    QStringList::Iterator host;

	    /* shorten hostname(s) - may be multiple (proxied) */
	    QStringList hosts = shortHost.split(QChar('@'));
	    for (host = hosts.begin(); host != hosts.end(); ++host) {
		/* decide whether or not to truncate this hostname */
		int dot = host->indexOf(QChar('.'));
		if (dot != -1)
		    /* no change if it looks even vaguely like an IP address */
		    if (!host->contains(QRegExp("^\\d+\\.")) &&	/* IPv4 */
			!host->contains(QChar(':')))		/* IPv6 */
			host->remove(dot, host->size());
	    }
	    host = hosts.begin();
	    shortHost = *host++;
	    for (; host != hosts.end(); ++host)
	        shortHost.append(QString("@")).append(*host);
	    titleString.replace(QRegExp("%h"), shortHost);
	    setTitle(titleString);
	}
	else 
	    setTitle(my.title);
    }
    else {
	if (hadTitle)
	    pmchart->updateHeight(-(titleLabel()->height()));
	setTitle(NULL);
    }
}

void Chart::changeTitle(QString title, int expand)
{
    changeTitle((char *)(const char *)title.toAscii(), expand);
}

QString Chart::scheme() const
{
    return my.scheme;
}

void Chart::setScheme(QString scheme)
{
    my.sequence = 0;
    my.scheme = scheme;
}

int Chart::sequence()
{
    return my.sequence;
}

void Chart::setSequence(int sequence)
{
    my.sequence = sequence;
}

void Chart::setScheme(QString scheme, int sequence)
{
    my.sequence = sequence;
    my.scheme = scheme;
}

Chart::Style Chart::style()
{
    return my.style;
}

void Chart::setStyle(Style style)
{
    my.style = style;
}

void Chart::setStroke(int i, Style style, QColor color)
{
    if (i < 0 || i >= my.items.size())
	abort();
    setStroke(my.items[i], style, color);
}

bool Chart::isStepped(ChartItem *item)
{
    int sem = item->my.metric->desc().desc().sem;
    return (sem == PM_SEM_INSTANT || sem == PM_SEM_DISCRETE);
}

void Chart::setStroke(ChartItem *item, Style style, QColor color)
{
    console->post("Chart::setStroke [style %d->%d]", my.style, style);

    setColor(item, color);

    QPen p(color);
    p.setWidth(8);
    item->my.curve->setLegendPen(p);
    item->my.curve->setRenderHint(QwtPlotItem::RenderAntialiased, my.antiAliasing);

    switch (style) {
	case BarStyle:
	    item->my.curve->setPen(color);
	    item->my.curve->setBrush(QBrush(color, Qt::SolidPattern));
	    item->my.curve->setStyle(QwtPlotCurve::Sticks);
	    if (my.style == UtilisationStyle)
		my.samplingScaleEngine->setAutoScale(true);
	    break;

	case AreaStyle:
	    item->my.curve->setPen(color);
	    item->my.curve->setBrush(QBrush(color, Qt::SolidPattern));
	    item->my.curve->setStyle(isStepped(item) ?
				  QwtPlotCurve::Steps : QwtPlotCurve::Lines);
	    if (my.style == UtilisationStyle)
		my.samplingScaleEngine->setAutoScale(true);
	    break;

	case UtilisationStyle:
	    item->my.curve->setPen(QColor(Qt::black));
	    item->my.curve->setStyle(QwtPlotCurve::Steps);
	    item->my.curve->setBrush(QBrush(color, Qt::SolidPattern));
	    my.samplingScaleEngine->setScale(false, 0.0, 100.0);
	    break;

	case LineStyle:
	    item->my.curve->setPen(color);
	    item->my.curve->setBrush(QBrush(Qt::NoBrush));
	    item->my.curve->setStyle(isStepped(item) ?
				  QwtPlotCurve::Steps : QwtPlotCurve::Lines);
	    if (my.style == UtilisationStyle)
		my.samplingScaleEngine->setAutoScale(true);
	    break;

	case StackStyle:
	    item->my.curve->setPen(QColor(Qt::black));
	    item->my.curve->setBrush(QBrush(color, Qt::SolidPattern));
	    item->my.curve->setStyle(QwtPlotCurve::Steps);
	    if (my.style == UtilisationStyle)
		my.samplingScaleEngine->setAutoScale(true);
	    break;

	case NoStyle:
	default:
	    abort();
    }

    // This is really quite difficult ... a Utilisation plot by definition
    // is dimensionless and scaled to a percentage, so a label of just
    // "% utilization" makes sense ... there has been some argument in
    // support of "% time utilization" as a special case when the metrics
    // involve some aspect of time, but the base metrics in the common case
    // are counters in units of time (e.g. the CPU view), which after rate
    // conversion is indistinguishable from instantaneous or discrete
    // metrics of dimension time^0 which are units compatible ... so we're
    // opting for the simplest possible interpretation of utilization or
    // everything else.
    //
    if (style == UtilisationStyle) {
	setYAxisTitle("% utilization");
    } else {
	setYAxisTitle(pmUnitsStr(&my.units));
    }

    if (style != my.style) {
	my.style = style;
	redoChartItems();
	replot();
    }
}

void Chart::redoChartItems(void)
{
    int		m;
    int		i;
    int		maxCount;
    double	sum;

    switch (my.style) {
	case BarStyle:
	case AreaStyle:
	case LineStyle:
	    for (m = 0; m < my.items.size(); m++) {
		for (i = 0; i < my.items[m]->my.dataCount; i++) {
		    my.items[m]->my.itemData[i] = my.items[m]->my.data[i];
		}
	    }
	    break;

	case UtilisationStyle:
	    maxCount = 0;
	    for (m = 0; m < my.items.size(); m++)
		maxCount = qMax(maxCount, my.items[m]->my.dataCount);
	    for (i = 0; i < maxCount; i++) {
		sum = 0.0;
		for (m = 0; m < my.items.size(); m++) {
		    if (i < my.items[m]->my.dataCount &&
			!SamplingCurve::isNaN(my.items[m]->my.data[i]))
			sum += my.items[m]->my.data[i];
		}
		for (m = 0; m < my.items.size(); m++) {
		    if (sum == 0.0 || i >= my.items[m]->my.dataCount || my.items[m]->my.hidden ||
			SamplingCurve::isNaN(my.items[0]->my.data[i]))
			my.items[m]->my.itemData[i] = SamplingCurve::NaN();
		    else
			my.items[m]->my.itemData[i] = 100 * my.items[m]->my.data[i] / sum;
		}
		sum = 0.0;
		for (m = 0; m < my.items.size(); m++) {
		    if (!SamplingCurve::isNaN(my.items[m]->my.itemData[i])) {
			sum += my.items[m]->my.itemData[i];
			my.items[m]->my.itemData[i] = sum;
		    }
		}
	    }
	    break;

	case StackStyle:
	    maxCount = 0;
	    for (m = 0; m < my.items.size(); m++)
		maxCount = qMax(maxCount, my.items[m]->my.dataCount);
	    for (i = 0; i < maxCount; i++) {
		for (m = 0; m < my.items.size(); m++) {
		    if (i >= my.items[m]->my.dataCount || my.items[m]->my.hidden)
			my.items[m]->my.itemData[i] = SamplingCurve::NaN();
		    else
			my.items[m]->my.itemData[i] = my.items[m]->my.data[i];
		}
		sum = 0.0;
		for (m = 0; m < my.items.size(); m++) {
		    if (!SamplingCurve::isNaN(my.items[m]->my.itemData[i])) {
			sum += my.items[m]->my.itemData[i];
			my.items[m]->my.itemData[i] = sum;
		    }
		}
	    }
	    break;

	case EventStyle:
	case NoStyle:
	    break;
    }
}

QColor Chart::color(int i)
{
    if (i >= 0 && i < my.items.size())
	return my.items[i]->my.color;
    return QColor("white");
}

void Chart::setColor(ChartItem *item, QColor c)
{
    item->my.color = c;
}

void Chart::setLabel(ChartItem *item, QString s)
{
    item->my.label = s;
}

void Chart::setLabel(int i, QString s)
{
    if (i >= 0 && i < my.items.size())
	setLabel(my.items[i], s);
}

void Chart::scale(bool *autoScale, double *yMin, double *yMax)
{
    if (my.eventType) {
	*autoScale = false;
	*yMin = 0.0;
	*yMax = 1.0;
    } else {
	*autoScale = my.samplingScaleEngine->autoScale();
	*yMin = my.samplingScaleEngine->minimum();
	*yMax = my.samplingScaleEngine->maximum();
    }
}

void Chart::setScale(bool autoScale, double yMin, double yMax)
{
    if (my.eventType) {
	my.tracingScaleEngine->setScale(autoScale, yMin, yMin);
    } else {
	my.samplingScaleEngine->setScale(autoScale, yMin, yMin);
	if (autoScale)
	    setAxisAutoScale(QwtPlot::yLeft);
	else
	    setAxisScale(QwtPlot::yLeft, yMin, yMax);
    }
    replot();
    redoScale();
}

bool Chart::rateConvert()
{
    return my.rateConvert;
}

void Chart::setRateConvert(bool rateConvert)
{
    my.rateConvert = rateConvert;
}

void Chart::setYAxisTitle(const char *p)
{
    QwtText *t;
    bool enable = (my.tab->currentGadget() == this);

    if (!p || *p == '\0')
	t = new QwtText(" ");	// for y-axis alignment (space is invisible)
    else
	t = new QwtText(p);
    t->setFont(*globalFont);
    t->setColor(enable ? globalSettings.chartHighlight : "black");
    setAxisTitle(QwtPlot::yLeft, *t);
}

void Chart::selected(const QwtDoublePoint &p)
{
    console->post("Chart::selected chart=%p x=%f y=%f", this, p.x(), p.y());
    my.tab->setCurrent(this);
    QString string;
    string.sprintf("[%.2f %s at %s]",
		   (float)p.y(), pmUnitsStr(&my.units), timeHiResString(p.x()));
    pmchart->setValueText(string);
}

void Chart::moved(const QwtDoublePoint &p)
{
    console->post("Chart::moved chart=%p x=%f y=%f ", this, p.x(), p.y());
    QString string;
    string.sprintf("[%.2f %s at %s]",
		   (float)p.y(), pmUnitsStr(&my.units), timeHiResString(p.x()));
    pmchart->setValueText(string);
}

bool Chart::legendVisible()
{
    // Legend is on or off for all items, only need to test the first item
    if (my.items.size() > 0)
	return legend() != NULL;
    return false;
}

// Use Edit->Chart Title and Legend to enable/disable the legend.
// Clicking on individual legend buttons will hide/show the
// corresponding item.
//
void Chart::setLegendVisible(bool on)
{
    console->post("Chart::setLegendVisible(%d) legend()=%p", on, legend());

    if (on) {
	if (legend() == NULL) {
	    // currently disabled, enable it
	    QwtLegend *l = new QwtLegend;
	    l->setItemMode(QwtLegend::CheckableItem);
	    l->setFrameStyle(QFrame::NoFrame);
	    l->setFrameShadow((Shadow)0);
	    l->setMidLineWidth(0);
	    l->setLineWidth(0);
	    insertLegend(l, QwtPlot::BottomLegend);
	    // force each Legend item to "checked" state matching
	    // the initial plotting state
	    for (int m = 0; m < my.items.size(); m++) {
		showItem(my.items[m]->my.curve, !my.items[m]->my.removed);
	    }
	}
    }
    else {
	QwtLegend *l = legend();
	if (l != NULL) {
	    // currently enabled, disable it
	    insertLegend(NULL, QwtPlot::BottomLegend);
	    // WISHLIST: this can cause a core dump - needs investigating
	    // [memleak].  Really, all of the legend code needs reworking.
	    // delete l;
	}
    }
}

void Chart::save(FILE *f, bool hostDynamic)
{
    SaveViewDialog::saveChart(f, this, hostDynamic);
}

void Chart::print(QPainter *qp, QRect &rect, bool transparent)
{
    QwtPlotPrintFilter filter;

    if (transparent)
	filter.setOptions(QwtPlotPrintFilter::PrintAll &
	    ~QwtPlotPrintFilter::PrintBackground &
	    ~QwtPlotPrintFilter::PrintGrid);
    else
	filter.setOptions(QwtPlotPrintFilter::PrintAll &
	    ~QwtPlotPrintFilter::PrintGrid);

    console->post("Chart::print: options=%d", filter.options());
    QwtPlot::print(qp, rect, filter);
}

bool Chart::antiAliasing()
{
    return my.antiAliasing;
}

void Chart::setAntiAliasing(bool on)
{
    console->post("Chart::setAntiAliasing [%d -> %d]", my.antiAliasing, on);
    my.antiAliasing = on;
}

QString Chart::name(int i) const
{
    return my.items[i]->my.name;
}

char *Chart::legendSpec(int i) const
{
    return my.items[i]->my.legend;
}

QmcDesc *Chart::metricDesc(int i) const
{
    return (QmcDesc *)&my.items[i]->my.metric->desc();
}

QString Chart::metricName(int i) const
{
    return my.items[i]->my.metric->name();
}

QString Chart::metricInstance(int i) const
{
    if (my.items[i]->my.metric->numInst() > 0)
	return my.items[i]->my.metric->instName(0);
    return QString::null;
}

QmcContext *Chart::metricContext(int i) const
{
    return my.items[i]->my.metric->context();
}

QmcMetric *Chart::metric(int i) const
{
    return my.items[i]->my.metric;
}

QSize Chart::minimumSizeHint() const
{
    return QSize(10,10);
}

QSize Chart::sizeHint() const
{
    return QSize(150,100);
}

void Chart::setupTree(QTreeWidget *tree)
{
    for (int i = 0; i < my.items.size(); i++) {
	ChartItem *item = my.items[i];
	if (!item->my.removed)
	    addToTree(tree, item->my.name,
		      item->my.metric->context(),
		      item->my.metric->hasInstances(), 
		      item->my.color, item->my.label);
    }
}

void Chart::addToTree(QTreeWidget *treeview, QString metric,
	const QmcContext *context, bool isInst, QColor color, QString label)
{
    QRegExp regexInstance("\\[(.*)\\]$");
    QRegExp regexNameNode(tr("\\."));
    QString source = context->source().source();
    QString inst, name = metric;
    QStringList	namelist;
    int depth;

    console->post("Chart::addToTree src=%s metric=%s, isInst=%d",
		(const char *)source.toAscii(), (const char *)metric.toAscii(),
		isInst);

    depth = name.indexOf(regexInstance);
    if (depth > 0) {
	inst = name.mid(depth+1);	// after '['
	inst.chop(1);			// final ']'
	name = name.mid(0, depth);	// prior '['
    }

    namelist = name.split(regexNameNode);
    namelist.prepend(source);	// add the host/archive root as well.
    if (depth > 0)
	namelist.append(inst);
    depth = namelist.size();

    // Walk through each component of this name, creating them in the
    // target tree (if not there already), right down to the leaf.

    NameSpace *tree = (NameSpace *)treeview->invisibleRootItem();
    NameSpace *item = NULL;

    for (int b = 0; b < depth; b++) {
	QString text = namelist.at(b);
	bool foundMatchingName = false;
	for (int i = 0; i < tree->childCount(); i++) {
	    item = (NameSpace *)tree->child(i);
	    if (text == item->text(0)) {
		// No insert at this level necessary, move down a level
		tree = item;
		foundMatchingName = true;
		break;
	    }
	}

	// When no more children and no match so far, we create & insert
	if (foundMatchingName == false) {
	    NameSpace *n;
	    if (b == 0) {
		n = new NameSpace(treeview, context);
		n->expand();
	        n->setExpanded(true, true);
		n->setSelectable(false);
	    }
	    else {
		bool isLeaf = (b == depth-1);
		n = new NameSpace(tree, text, isLeaf && isInst);
		if (isLeaf) {
		    n->setLabel(label);
		    n->setOriginalColor(color);
		    n->setCurrentColor(color, NULL);
		}
		n->expand();
	        n->setExpanded(!isLeaf, true);
		n->setSelectable(isLeaf);
		if (!isLeaf)
		    n->setType(NameSpace::NonLeafName);
		else if (isInst)	// Constructor sets Instance type
		    tree->setType(NameSpace::LeafWithIndom);
		else
		    n->setType(NameSpace::LeafNullIndom);
	    }
	    tree = n;
	}
    }
}

bool Chart::checkCompatibleUnits(pmUnits *newUnits)
{
    console->post("Chart::check units plot units %s", pmUnitsStr(newUnits));
    if (my.units.dimSpace != newUnits->dimSpace ||
        my.units.dimTime != newUnits->dimTime ||
        my.units.dimCount != newUnits->dimCount)
	return false;
    return true;
}

bool Chart::checkCompatibleTypes(int newType)
{
    console->post("Chart::check plot event type %s", pmTypeStr(newType));
    if (my.eventType == true && newType != PM_TYPE_EVENT)
	return false;
    if (my.eventType == false && newType == PM_TYPE_EVENT)
	return false;
    return true;
}

bool Chart::activeItem(int i)
{
    if (i >= 0 && i < my.items.size())
	return (my.items[i]->my.removed == false);
    return false;
}
