{
    "$schema": "https://vega.github.io/schema/vega/v5.0.json",
    "width": 1000,
    "height": 370,
    "config": {
        "text": {
            "font": "Noto Sans Variable"
        }
    },

    "signals": [
        {
            "name": "selected",
            "value": "",
            "on": [{"events": "mouseover", "update": "datum"}]
        },
        {
            "name": "rowHeight",
            "value": 24
        },
        {
            "name": "radius",
            "update":
            "120"
        }
    ],

    "data": [
        {
            "name": "kinds",
            "values": @KINDS@,
            "transform": [
                {
                    "type": "pie",
                    "field": "count"
                }
            ]
        },
        {
            "name": "paths",
            "values": @PATHS@,
            "transform": [
                {
                    "type": "window",
                    "ops": ["row_number"],
                    "as": ["id"]
                }
            ]
        },
        {
            "name": "names",
            "values": @NAMES@,
            "transform": [
                {
                    "type": "window",
                    "ops": ["row_number"],
                    "as": ["id"]
                }
            ]
        },
        {
            "name": "severities",
            "values": @SEVERITIES@
        }
    ],

    "scales": [
        {
            "name": "color",
            "type": "ordinal",
            "range": ["red", "orange", "blue"],
            "domain": ["Violation", "Suppressed", "Deviation"]
        },
        {
            "name": "angular",
            "type": "point",
            "range": {"signal": "[-PI, PI]"},
            "padding": 0.5,
            "domain": {"data": "severities", "field": "severity"}
        },
        {
            "name": "radial",
            "type": "linear",
            "range": {"signal": "[0, radius]"},
            "zero": true,
            "nice": false,
            "domain": [0, @NUM_RULES@]
        }
    ],

    "marks": [
        {
            "name": "compliance-chart",
            "type": "group",
            "encode": {
                "enter": {
                    "x": {
                        "signal": "radius",
                        "offset": {"value": 40}
                    },
                    "y": {
                        "signal": "radius",
                        "offset": {"value": 20}
                    }
                }
            },
            "legends": [
                {
                    "fill": "color",
                    "title": "Legends",
                    "orient": "none",
                    "encode": {
                        "legend": {
                            "enter": {
                                "x": {
                                    "signal": "radius * 1.05",
                                    "offset": {"signal": "10"}
                                },
                                "y": {"offset": -35}
                            }
                        }
                    }
                }
            ],
            "marks": [
                {
                    "type": "arc",
                    "from": {"data": "kinds"},
                    "encode": {
                        "enter": {
                            "startAngle": {"field": "startAngle"},
                            "endAngle": {"field": "endAngle"},
                            "padAngle": {"value": "0.08"},
                            "fill": {"scale": "color", "field": "kind"},
                            "stroke": {"signal": "scale('color', datum.kind)"},
                            "strokeWidth": {"value": 2},
                            "fillOpacity": {"value": 0.8},
                            "href": {"signal": "datum.url"}
                        },
                        "update": {
                            "innerRadius": {"signal": "if(selected && selected.kind == datum.kind, radius * 0.6, radius * 0.65)"},
                            "outerRadius": {"signal": "if(selected && selected.kind == datum.kind, radius * 1.05, radius)"}
                        }
                    }
                },
                {
                    "type": "text",
                    "encode": {
                        "enter": {
                            "x": {
                                "signal": "radius * 1.05"
                            },
                            "y": {"offset": -80},
                            "fontSize": {"value": 18},
                            "align": {"value": "left"},
                            "text": {"value": "Compliance Report"}
                        }
                    }
                },
                {
                    "type": "text",
                    "from": {"data": "kinds"},
                    "encode": {
                        "enter": {
                            "text": {
                                "signal": "if(datum.endAngle - datum.startAngle < 0.3, '', datum.count)"
                            },
                            "radius": {"signal": "radius * 0.825"},
                            "theta": {"signal": "(datum.startAngle + datum.endAngle)/2"},
                            "fill": {"value": "white"},
                            "fontSize": {"value": 12},
                            "align": {"value": "center"},
                            "baseline": {"value": "middle"},
                            "href": {"signal": "datum.url"}
                        }
                    }
                },
                {
                    "type": "image",
                    "encode": {
                        "enter": {
                            "url": {"value": "@COMPLIANCE_CHART_ICON@"},
                            "width": {"value": 100},
                            "align": {"value": "center"},
                            "baseline": {"value": "middle"}
                        }
                    }
                },
                {
                    "type": "rect",
                    "encode": {
                        "enter": {
                            "x": {"value": -50},
                            "y": {"value": -50},
                            "width": {"value": 100},
                            "height": {"value": 100},
                            "fill": {"value": "transparent"},
                            "tooltip": {"signal": "{'title': '@COMPLIANCE_CHART_ICON_TOOLTIP@', 'hint': 'Click to open in issue table'}"},
                            "href": {"value": "@COMPLIANCE_CHART_ICON_URL@"}
                        }
                    }
                }
            ]
        },
        {
            "name": "violated-rules",
            "type": "group",
            "encode": {
                "enter": {
                    "x": {
                        "signal": "width / 2",
                        "offset": {"value": 250}
                    },
                    "y": {
                        "signal": "radius",
                        "offset": {"value": 20}
                    }
                }
            },
            "marks": [
                {
                    "type": "text",
                    "encode": {
                        "enter": {
                            "x": {"signal": "radius * 0.5"},
                            "y": {"offset": -80},
                            "fontSize": {"value": 18},
                            "align": {"value": "left"},
                            "text": {"value": "Violated Rules"}
                        }
                    }
                },
                {
                    "type": "line",
                    "from": {"data": "severities"},
                    "encode": {
                        "enter": {
                            "interpolate": {"value": "linear-closed"},
                            "x": {"signal": "scale('radial', datum.count) * cos(scale('angular', datum.severity))"},
                            "y": {"signal": "scale('radial', datum.count) * sin(scale('angular', datum.severity))"},
                            "stroke": {"value": "blue"},
                            "strokeWidth": {"value": 1},
                            "fill": {"value": "blue"},
                            "fillOpacity": {"value": 0.1}
                        }
                    }
                },
                {
                    "type": "rule",
                    "name": "radial-grid",
                    "from": {"data": "severities"},
                    "encode": {
                        "enter": {
                            "x": {"value": 0},
                            "y": {"value": 0},
                            "x2": {"signal": "radius * cos(scale('angular', datum.severity))"},
                            "y2": {"signal": "radius * sin(scale('angular', datum.severity))"},
                            "stroke": {"value": "lightgray"},
                            "strokeWidth": {"value": 1}
                        }
                    }
                },
                {
                    "type": "text",
                    "from": {"data": "severities"},
                    "encode": {
                        "enter": {
                            "x": {"signal": "(radius + 5) * cos(scale('angular', datum.severity))"},
                            "y": {"signal": "(radius + 5) * sin(scale('angular', datum.severity))"},
                            "text": {"signal": "datum.severity + ' (' + datum.count + '/@NUM_RULES@)'"},
                            "align": [
                                {"test": "abs(scale('angular', datum.severity)) > PI / 2", "value": "center"},
                                {"value": "left"}
                            ],
                            "baseline": [
                                {"test": "scale('angular', datum.severity) > 0", "value": "top"},
                                {"test": "scale('angular', datum.severity) == 0", "value": "middle"},
                                {"value": "bottom"}
                            ],
                            "fontSize": {"value": 12},
                            "href": {"signal": "datum.url"},
                            "tooltip": {"signal": "{'title': datum.name, 'hint': 'Click to open in issue table'}"}
                        }
                    }
                },
                {
                    "type": "line",
                    "from": {"data": "radial-grid"},
                    "encode": {
                        "enter": {
                            "interpolate": {"value": "linear-closed"},
                            "x": {"field": "x2"},
                            "y": {"field": "y2"},
                            "stroke": {"value": "lightgray"},
                            "strokeWidth": {"value": 1}
                        }
                    }
                }
            ]
        },
        {
            "name": "top-files",
            "type": "group",
            "encode": {
                "enter": {
                    "x": {"value": -120},
                    "y": {"value": 330}
                }
            },
            "marks": [
                {
                    "type": "text",
                    "encode": {
                        "enter": {
                            "x": {"value": 300},
                            "y": {"value": -15},
                            "fontSize": {"value": 18},
                            "align": {"value": "center"},
                            "text": {"value": "Top 5 Affected Files"}
                        }
                    }
                },
                {
                    "type": "rect",
                    "from": {"data": "paths"},
                    "encode": {
                        "enter": {
                            "y": {"signal": "rowHeight * datum.id - rowHeight"},
                            "width": {"value": 600},
                            "height": {"signal": "rowHeight"},
                            "fill": {"value": "white"},
                            "stroke": {"value": "#ddd"},
                            "href": {"signal": "datum.url"},
                            "tooltip": {"signal": "{'title': datum.path, 'hint': 'Click to open file in editor'}"}
                        }
                    }
                },
                {
                    "type": "text",
                    "from": {"data": "paths"},
                    "encode": {
                        "enter": {
                            "x": {"value": 10},
                            "y": {"signal": "rowHeight * datum.id - rowHeight/2"},
                            "text": {"field": "path"},
                            "baseline": {"value": "middle"},
                            "fontSize": {"value": 14},
                            "align": {"value": "left"},
                            "href": {"signal": "datum.url"},
                            "tooltip": {"signal": "{'title': datum.path, 'hint': 'Click to open file in editor'}"}
                        }
                    }
                },
                {
                    "type": "text",
                    "from": {"data": "paths"},
                    "encode": {
                        "enter": {
                            "x": {"value": 590},
                            "y": {"signal": "rowHeight * datum.id - rowHeight/2"},
                            "text": {"field": "count"},
                            "baseline": {"value": "middle"},
                            "fontSize": {"value": 14},
                            "align": {"value": "right"},
                            "href": {"signal": "datum.url"},
                            "tooltip": {"signal": "{'title': datum.path, 'hint': 'Click to open file in editor'}"}
                        }
                    }
                }
            ]
        },
        {
            "name": "top-issues",
            "type": "group",
            "encode": {
                "enter": {
                    "x": {"value": 550},
                    "y": {"value": 330}
                }
            },
            "marks": [
                {
                    "type": "text",
                    "encode": {
                        "enter": {
                            "x": {"value": 225},
                            "y": {"value": -15},
                            "fontSize": {"value": 18},
                            "align": {"value": "center"},
                            "text": {"value": "Top 5 Issues"}
                        }
                    }
                },
                {
                    "type": "rect",
                    "from": {"data": "names"},
                    "encode": {
                        "enter": {
                            "y": {"signal": "rowHeight * datum.id - rowHeight"},
                            "width": {"value": 450},
                            "height": {"signal": "rowHeight"},
                            "fill": {"value": "white"},
                            "stroke": {"value": "#ddd"},
                            "href": {"signal": "datum.url"},
                            "tooltip": {"signal": "{'title': datum.name, 'hint': 'Click to open in issue table'}"}
                        }
                    }
                },
                {
                    "type": "text",
                    "from": {"data": "names"},
                    "encode": {
                        "enter": {
                            "y": {"signal": "rowHeight * datum.id - rowHeight/2"},
                            "x": {"value": 10},
                            "text": {"field": "name"},
                            "baseline": {"value": "middle"},
                            "fontSize": {"value": 14},
                            "align": {"value": "left"},
                            "href": {"signal": "datum.url"},
                            "tooltip": {"signal": "{'title': datum.name, 'hint': 'Click to open in issue table'}"}
                        }
                    }
                },
                {
                    "type": "text",
                    "from": {"data": "names"},
                    "encode": {
                        "enter": {
                            "x": {"value": 300},
                            "y": {"signal": "rowHeight * datum.id - rowHeight/2"},
                            "text": {"field": "severity"},
                            "baseline": {"value": "middle"},
                            "fontSize": {"value": 14},
                            "align": {"value": "left"},
                            "href": {"signal": "datum.url"},
                            "tooltip": {"signal": "{'title': datum.name, 'hint': 'Click to open in issue table'}"}
                        }
                    }
                },
                {
                    "type": "text",
                    "from": {"data": "names"},
                    "encode": {
                        "enter": {
                            "x": {"value": 440},
                            "y": {"signal": "rowHeight * datum.id - rowHeight/2"},
                            "text": {"field": "count"},
                            "baseline": {"value": "middle"},
                            "fontSize": {"value": 14},
                            "align": {"value": "right"},
                            "href": {"signal": "datum.url"},
                            "tooltip": {"signal": "{'title': datum.name, 'hint': 'Click to open in issue table'}"}
                        }
                    }
                }
            ]
        }
    ]
}
