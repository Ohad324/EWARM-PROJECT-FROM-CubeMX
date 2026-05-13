{
    "$schema": "https://vega.github.io/schema/vega/v5.json",
    "description": "Tree-map",
    "width": 800,
    "height": 400,
    "autosize": "none",
    "config": {
        "text": {
            "font": "Noto Sans Variable"
        }
    },

    "signals": [
        {
            "name": "Layout", "value": "resquarify",
            "bind": {
                "input": "select",
                "options": [
                    "binary",
                    "dice",
                    "resquarify",
                    "slice",
                    "slicedice",
                    "squarify"
                ]
            }
        },
        {
            "name": "Ratio", "value": 1,
            "bind": {"input": "range", "min": 1, "max": 5, "step": 0.1}
        },
        {
            "name": "Inner", "value": 3,
            "bind": {"input": "range", "min": 0, "max": 10, "step": 1}
        },
        {
            "name": "Outer", "value": 5,
            "bind": {"input": "range", "min": 0, "max": 10, "step": 1}
        },
        {
            "name": "White", "value": @WHITE@,
            "bind": {"input": "range", "min": 0, "max": 4999, "step": 10}
        },
        {
            "name": "Red", "value": @RED@,
            "bind": {"input": "range", "min": 1, "max": 5000, "step": 10}
        }
    ],

    "data": [
        {
            "name": "tree",
            "values": @VALUES@,
            "transform": [
                {
                    "type": "stratify",
                    "key": "id",
                    "parentKey": "parent"
                },
                {
                    "type": "treemap",
                    "field": "size",
                    "sort": {"field": "value"},
                    "round": true,
                    "paddingTop": 18,
                    "method": {"signal": "Layout"},
                    "ratio": {"signal": "Ratio"},
                    "paddingInner": {"signal": "Inner"},
                    "paddingOuter": {"signal": "Outer"},
                    "size": [{"signal": "width"}, {"signal": "height"}]
                }
            ]
        },
        {
            "name": "nodes",
            "source": "tree",
            "transform": [{ "type": "filter", "expr": "datum.children" }]
        },
        {
            "name": "leaves",
            "source": "tree",
            "transform": [{ "type": "filter", "expr": "!datum.children" }]
        }
    ],

    "scales": [
        {
            "name": "color",
            "type": "linear",
            "domain": [{"signal": "White"}, {"signal": "Red"}],
            "range": ["white", "red"]
        }
    ],

    "marks": [
        {
            "type": "rect",
            "from": {"data": "nodes"},
            "interactive": false,
            "encode": {
                "enter": {
                    "stroke": {"value": "black"},
                    "fill": {"value": "#f8f8f8"}
                },
                "update": {
                    "x": {"field": "x0"},
                    "y": {"field": "y0"},
                    "x2": {"field": "x1"},
                    "y2": {"field": "y1"}
                }
            }
        },
        {
            "type": "rect",
            "from": {"data": "leaves"},
            "encode": {
                "enter": {
                    "stroke": {"value": "black"},
                    "tooltip": {"signal": "{'title': datum.name, 'Path': datum.path, '@SIZE_TOOLTIP@': datum.size, '@COLOR_TOOLTIP@': datum.color}"}
                },
                "update": {
                    "x": {"field": "x0"},
                    "y": {"field": "y0"},
                    "x2": {"field": "x1"},
                    "y2": {"field": "y1"},
                    "fill": {"scale": "color", "field": "color"},
                    "href": {"signal": "datum.url"}
                },
                "hover": {
                    "fill": {"value": "blue"}
                }
            }
        },
        {
            "type": "text",
            "from": {"data": "nodes"},
            "interactive": false,
            "encode": {
                "enter": {
                    "align": {"value": "left"},
                    "baseline": {"value": "top"},
                    "fill": {"value": "#000"},
                    "text": {"field": "name"},
                    "fontSize": {"value": 14}
                },
                "update": {
                    "x": {"signal": "datum.x0 + 2"},
                    "y": {"signal": "datum.y0 + 2"},
                    "limit": {"signal": "datum.x1 - datum.x0 - 2"}
                }
            }
        }
    ]
}
