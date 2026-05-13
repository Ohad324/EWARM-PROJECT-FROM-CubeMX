{
    "$schema": "https://vega.github.io/schema/vega/v5.0.json",
    "width": 400,
    "height": 400,
    "config": {
        "text": {
            "font": "Noto Sans Variable"
        }
    },
    "data": [
        {
            "name": "objects",
            "values": @VALUES@,
            "transform": [
                {
                    "type": "collect",
                    "sort": {
                        "field": ["category", "subcategory"],
                        "order": ["ascending", "ascending"]
                    }
                },
                {
                    "type": "joinaggregate",
                    "fields": ["count"],
                    "ops": ["sum"],
                    "as": ["total"]
                },
                {
                    "type": "formula",
                    "expr": "round(datum.count / datum.total * 1000) / 10",
                    "as": "percent"
                }
            ]
        },
        {
            "name": "total",
            "source": "objects",
            "transform": [
                {
                    "type": "aggregate",
                    "fields": ["count"],
                    "ops": ["sum"],
                    "as": ["sum_count"]
                }
            ]
        },
        {
            "name": "categories",
            "source": "objects",
            "transform": [
                {
                    "type": "aggregate",
                    "groupby": ["category"],
                    "fields": ["percent", "count"],
                    "ops": ["sum", "sum"],
                    "as": ["sum_percent_category", "sum_count_category"]
                },
                {
                    "type": "pie",
                    "field": "sum_percent_category",
                    "as": ["category_start_angle", "category_end_angle"]
                },
                {
                    "type": "formula",
                    "expr": "round(datum.sum_percent_category * 10) / 10",
                    "as": "sum_percent_category"
                }
            ]
        },
        {
            "name": "subcategories",
            "source": "objects",
            "transform": [
                {
                    "type": "pie",
                    "field": "percent",
                    "as": ["subcategory_start_angle", "subcategory_end_angle"]
                }
            ]
        }
    ],

    "scales": [
        {
            "name": "color",
            "type": "ordinal",
            "range": @CATEGORY_COLORS@,
            "domain": @CATEGORIES@
        }
    ],

    "marks": [
        {
            "type": "text",
            "from": {"data": "total"},
            "encode": {
                "enter": {
                    "text": {"signal": "'Total: ' + datum.sum_count"},
                    "x": {"signal": "width / 2"},
                    "y": {"signal": "height / 2"},
                    "fill": {"value": "black"},
                    "fontSize": {"value": 14},
                    "align": {"value": "center"},
                    "baseline": {"value": "middle"}
                }
            }
        },
        {
            "type": "arc",
            "from": {"data": "categories"},
            "encode": {
                "enter": {
                    "x": {"signal": "width / 2"},
                    "y": {"signal": "height / 2"},
                    "fill": {"scale": "color",  "field": "category"},
                    "fillOpacity": {"value": 0.8},
                    "stroke": {"value": "white"},
                    "startAngle": {"field": "category_start_angle"},
                    "endAngle": {"field": "category_end_angle"},
                    "innerRadius": {"value": 80},
                    "outerRadius": {"value": 100},
                    "tooltip": {"signal": "{'title': datum.category, 'Count': datum.sum_count_category, 'Percent': datum.sum_percent_category + '%', 'hint': 'Click to open in issue table'}"},
                    "href": {"signal": "'@CATEGORY_URL@\"' + datum.category + '\"'"}
                }
            }
        },
        {
            "type": "arc",
            "from": {"data": "subcategories"},
            "encode": {
                "enter": {
                    "x": {"signal": "width / 2"},
                    "y": {"signal": "height / 2"},
                    "fill": {"scale": "color", "field": "category"},
                    "fillOpacity": {"signal": "min(max(datum.percent * 0.04, 0.2), 0.7)"},
                    "stroke": {"value": "white"},
                    "startAngle": {"field": "subcategory_start_angle"},
                    "endAngle": {"field": "subcategory_end_angle"},
                    "innerRadius": {"value": 100},
                    "outerRadius": {"value": 150},
                    "tooltip": {"signal": "{'title': datum.subcategory, '@CATEGORY_TITLE@': datum.category, 'Count': datum.count, 'Percent': datum.percent + '%', 'hint': 'Click to open in issue table'}"},
                    "href": {"signal": "datum.url"}
                }
            }
        },
        {
            "type": "text",
            "from": {"data": "subcategories"},
            "encode": {
                "enter": {
                    "text": {"signal": "datum.percent < 3 ? '' : datum.percent + '%'"},
                    "x": {"signal": "width / 2"},
                    "y": {"signal": "height / 2"},
                    "radius": {"value": 125},
                    "theta": {"signal": "(datum.subcategory_start_angle + datum.subcategory_end_angle)/2"},
                    "fill": {"value": "black"},
                    "fontSize": {"value": 12},
                    "align": {"value": "center"},
                    "baseline": {"value": "middle"},
                    "tooltip": {"signal": "{'title': datum.subcategory, '@CATEGORY_TITLE@': datum.category, 'Count': datum.count, 'Percent': datum.percent + '%', 'hint': 'Click to open in issue table'}"},
                    "href": {"signal": "datum.url"}
                }
            }
        },
        {
            "type": "text",
            "from": {"data": "subcategories"},
            "encode": {
                "enter": {
                    "text": {"signal": "datum.percent < 3 ? '' : datum.subcategory"},
                    "x": {"signal": "width / 2"},
                    "y": {"signal": "height / 2"},
                    "radius": {"value": 170},
                    "theta": {"signal": "(datum.subcategory_start_angle + datum.subcategory_end_angle)/2"},
                    "fill": {"value": "grey"},
                    "fontSize": {"value": 14},
                    "align": {"value": "center"},
                    "baseline": {"value": "middle"},
                    "tooltip": {"signal": "{'title': datum.subcategory, '@CATEGORY_TITLE@': datum.category, 'Count': datum.count, 'Percent': datum.percent + '%', 'hint': 'Click to open in issue table'}"},
                    "href": {"signal": "datum.url"}
                }
            }
        }
    ],

    "legends": [
        {
            "fill": "color",
            "title": "@CATEGORY_TITLE@",
            "orient": "right",
            "encode": {
                "symbols": {
                    "enter": {
                        "fillOpacity": {"value": 0.5}
                    }
                },
                "labels": {
                    "enter": {
                        "text": {"field": "value"},
                        "tooltip": {"signal": "{'title': datum.value, 'hint': 'Click to open in issue table'}"},
                        "href": {"signal": "'@CATEGORY_URL@\"' + datum.value + '\"'"}
                    }
                }
            }
        }
    ]
}
