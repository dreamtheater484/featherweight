"""Generates the chart document of the public corpus (plan D5, fwchart):

  generated/charts.docx  charts as Word 2016 writes them, from their cached values (no workbook):
                         clustered and percent-stacked columns, stacked bars with data labels,
                         lines with markers, a pie with percent labels, a doughnut, areas, a
                         scatter chart, and a radar chart, which Featherweight leaves a placeholder

The XML is written by hand, so this needs only the standard library, and the output is
deterministic. From the repository root:

  python tests/office/make_chart_tests.py

The chart parts are also fuzz_chart's seeds (tests/fuzz/seeds/chart).
"""
import os
import zipfile

import make_drawing_tests as dt

ROOT = os.path.dirname(os.path.abspath(__file__))
SEEDS = os.path.join(ROOT, "..", "fuzz", "seeds", "chart")
CHART_URI = "http://schemas.openxmlformats.org/drawingml/2006/chart"
CHART_TYPE = "application/vnd.openxmlformats-officedocument.drawingml.chart+xml"

HEAD = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
        '<c:chartSpace xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart" '
        'xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" '
        'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">'
        '<c:date1904 val="0"/><c:lang val="en-US"/><c:roundedCorners val="0"/>'
        '<mc:AlternateContent xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006">'
        '<mc:Choice Requires="c14" xmlns:c14="http://schemas.microsoft.com/office/drawing/2007/8/2/chart">'
        '<c14:style val="102"/></mc:Choice><mc:Fallback><c:style val="2"/></mc:Fallback></mc:AlternateContent>')
GREY = '<a:solidFill><a:schemeClr val="tx1"><a:lumMod val="65000"/><a:lumOff val="35000"/></a:schemeClr></a:solidFill>'
RULE = ('<a:ln w="9525" cap="flat" cmpd="sng" algn="ctr"><a:solidFill><a:schemeClr val="tx1"><a:lumMod val="15000"/>'
        '<a:lumOff val="85000"/></a:schemeClr></a:solidFill><a:round/></a:ln>')
NONE = '<c:spPr><a:noFill/><a:ln><a:noFill/></a:ln><a:effectLst/></c:spPr>'

TITLE_RPR = ' spc="0" baseline="0"'
AXIS_BODY = ' rot="-60000000" vert="horz"'


def txpr(size, extra="", rot=""):
    return (f'<c:txPr><a:bodyPr{rot}/><a:lstStyle/><a:p><a:pPr><a:defRPr sz="{size}" b="0" i="0" u="none" strike="noStrike" '
            f'kern="1200"{extra}>{GREY}<a:latin typeface="+mn-lt"/><a:ea typeface="+mn-ea"/><a:cs typeface="+mn-cs"/>'
            '</a:defRPr></a:pPr><a:endParaRPr lang="en-US"/></a:p></c:txPr>')


def title(text=None, size=1400):
    tx = ""
    if text:
        tx = (f'<c:tx><c:rich><a:bodyPr/><a:lstStyle/><a:p><a:pPr><a:defRPr sz="{size}"/></a:pPr><a:r><a:rPr lang="en-US"/>'
              f'<a:t>{text}</a:t></a:r></a:p></c:rich></c:tx>')
    return f'<c:title>{tx}<c:overlay val="0"/>{NONE}{txpr(size, TITLE_RPR)}</c:title>'


def cache(kind, values, fmt=None):
    head = f"<c:formatCode>{fmt}</c:formatCode>" if fmt else ""
    pts = "".join(f'<c:pt idx="{i}"><c:v>{v}</c:v></c:pt>' for i, v in enumerate(values) if v is not None)
    return f'<c:{kind}>{head}<c:ptCount val="{len(values)}"/>{pts}</c:{kind}>'


def ser(i, name, cats, vals, look, extra="", xy=False, col="B"):
    end = len(vals) + 1
    cat_tag, val_tag = ("xVal", "yVal") if xy else ("cat", "val")
    cat_ref = (f'<c:numRef><c:f>Sheet1!$A$2:$A${end}</c:f>{cache("numCache", cats, "General")}</c:numRef>' if xy else
               f'<c:strRef><c:f>Sheet1!$A$2:$A${end}</c:f>{cache("strCache", cats)}</c:strRef>')
    return (f'<c:ser><c:idx val="{i}"/><c:order val="{i}"/><c:tx><c:strRef><c:f>Sheet1!${col}$1</c:f>'
            f'{cache("strCache", [name])}</c:strRef></c:tx>{look}{extra}<c:{cat_tag}>{cat_ref}</c:{cat_tag}>'
            f'<c:{val_tag}><c:numRef><c:f>Sheet1!${col}$2:${col}${end}</c:f>{cache("numCache", vals, "General")}'
            f'</c:numRef></c:{val_tag}></c:ser>')


def fill(i):
    return f'<c:spPr><a:solidFill><a:schemeClr val="accent{i + 1}"/></a:solidFill><a:ln><a:noFill/></a:ln><a:effectLst/></c:spPr>'


def stroke(i, width=28575):
    return (f'<c:spPr><a:ln w="{width}" cap="rnd"><a:solidFill><a:schemeClr val="accent{i + 1}"/></a:solidFill>'
            '<a:round/></a:ln><a:effectLst/></c:spPr>')


def marker(i, symbol="circle"):
    return (f'<c:marker><c:symbol val="{symbol}"/><c:size val="5"/><c:spPr><a:solidFill><a:schemeClr val="accent{i + 1}"/>'
            f'</a:solidFill><a:ln w="9525"><a:solidFill><a:schemeClr val="accent{i + 1}"/></a:solidFill></a:ln>'
            '<a:effectLst/></c:spPr></c:marker>')


def labels(val=False, percent=False, cat=False, pos=None):
    p = f'<c:dLblPos val="{pos}"/>' if pos else ""
    return (f'<c:dLbls>{NONE}{txpr(900)}{p}<c:showLegendKey val="0"/><c:showVal val="{int(val)}"/>'
            f'<c:showCatName val="{int(cat)}"/><c:showSerName val="0"/><c:showPercent val="{int(percent)}"/>'
            '<c:showBubbleSize val="0"/></c:dLbls>')


NO_LABELS = ('<c:dLbls><c:showLegendKey val="0"/><c:showVal val="0"/><c:showCatName val="0"/><c:showSerName val="0"/>'
             '<c:showPercent val="0"/><c:showBubbleSize val="0"/></c:dLbls>')


def axis(tag, ax, cross, pos, fmt="General", grid=False, extra="", delete=False):
    g = f'<c:majorGridlines><c:spPr>{RULE}<a:effectLst/></c:spPr></c:majorGridlines>' if grid else ""
    line = f'<c:spPr><a:noFill/>{RULE}<a:effectLst/></c:spPr>' if tag == "catAx" else NONE
    tail = ('<c:crosses val="autoZero"/><c:auto val="1"/><c:lblAlgn val="ctr"/><c:lblOffset val="100"/>'
            '<c:noMultiLvlLbl val="0"/>' if tag == "catAx" else
            f'<c:crosses val="autoZero"/>{extra}')
    return (f'<c:{tag}><c:axId val="{ax}"/><c:scaling><c:orientation val="minMax"/></c:scaling><c:delete val="{int(delete)}"/>'
            f'<c:axPos val="{pos}"/>{g}<c:numFmt formatCode="{fmt}" sourceLinked="1"/><c:majorTickMark val="none"/>'
            f'<c:minorTickMark val="none"/><c:tickLblPos val="nextTo"/>{line}'
            f'{txpr(900, "", AXIS_BODY)}<c:crossAx val="{cross}"/>{tail}</c:{tag}>')


def chart(plot, title_xml=title(), legend="b", axes=""):
    leg = (f'<c:legend><c:legendPos val="{legend}"/><c:overlay val="0"/>{NONE}{txpr(900)}</c:legend>' if legend else "")
    return (HEAD + f'<c:chart>{title_xml}<c:autoTitleDeleted val="{0 if title_xml else 1}"/><c:plotArea><c:layout/>'
            f'{plot}{axes}{NONE}</c:plotArea>{leg}<c:plotVisOnly val="1"/><c:dispBlanksAs val="gap"/></c:chart>'
            '<c:spPr><a:solidFill><a:schemeClr val="bg1"/></a:solidFill><a:ln w="9525" cap="flat" cmpd="sng" algn="ctr">'
            '<a:noFill/><a:round/></a:ln><a:effectLst/></c:spPr><c:txPr><a:bodyPr/><a:lstStyle/><a:p><a:pPr><a:defRPr/>'
            '</a:pPr><a:endParaRPr lang="en-US"/></a:p></c:txPr></c:chartSpace>')


CATS = ["Category 1", "Category 2", "Category 3", "Category 4"]
SERIES = [("Series 1", [4.3, 2.5, 3.5, 4.5]), ("Series 2", [2.4, 4.4, 1.8, 2.8]), ("Series 3", [2, 2, 3, 5])]
VAL_AXIS = '<c:crossBetween val="between"/>'


def bars(direction, grouping, overlap, gap, series_extra=lambda i: "", group_labels=NO_LABELS, fmt="General", t=title()):
    cat_pos, val_pos = ("b", "l") if direction == "col" else ("l", "b")
    s = "".join(ser(i, n, CATS, v, fill(i), '<c:invertIfNegative val="0"/>' + series_extra(i), col="BCD"[i])
                for i, (n, v) in enumerate(SERIES))
    plot = (f'<c:barChart><c:barDir val="{direction}"/><c:grouping val="{grouping}"/><c:varyColors val="0"/>{s}'
            f'{group_labels}<c:gapWidth val="{gap}"/><c:overlap val="{overlap}"/><c:axId val="101"/><c:axId val="102"/>'
            '</c:barChart>')
    return chart(plot, t, axes=axis("catAx", 101, 102, cat_pos) + axis("valAx", 102, 101, val_pos, fmt, True, VAL_AXIS))


def charts():
    out = {}
    out["column"] = bars("col", "clustered", -27, 219)
    out["bar-stacked"] = bars("bar", "stacked", 100, 150, group_labels=labels(val=True, pos=None),
                              t=title("Stacked bars, values shown"))
    out["column-percent"] = bars("col", "percentStacked", 100, 150, fmt="0%", t=title("Percent of the total"))
    lines = "".join(ser(i, n, CATS, v, stroke(i), marker(i), col="BCD"[i]) for i, (n, v) in enumerate(SERIES))
    out["line"] = chart(f'<c:lineChart><c:grouping val="standard"/><c:varyColors val="0"/>{lines}{NO_LABELS}'
                        '<c:marker val="1"/><c:smooth val="0"/><c:axId val="201"/><c:axId val="202"/></c:lineChart>',
                        title("Lines with markers"),
                        axes=axis("catAx", 201, 202, "b") + axis("valAx", 202, 201, "l", "0.0", True, VAL_AXIS))
    sales = ["1st Qtr", "2nd Qtr", "3rd Qtr", "4th Qtr"]
    pts = "".join(f'<c:dPt><c:idx val="{i}"/><c:bubble3D val="0"/><c:spPr><a:solidFill><a:schemeClr val="accent{i + 1}"/>'
                  '</a:solidFill><a:ln w="19050"><a:solidFill><a:schemeClr val="lt1"/></a:solidFill></a:ln><a:effectLst/>'
                  '</c:spPr></c:dPt>' for i in range(4))
    out["pie"] = chart('<c:pieChart><c:varyColors val="1"/>' +
                       ser(0, "Sales", sales, [8.2, 3.2, 1.4, 1.2], "", pts + labels(percent=True, pos="bestFit")) +
                       '<c:firstSliceAng val="0"/></c:pieChart>', title(), legend="r")
    out["doughnut"] = chart('<c:doughnutChart><c:varyColors val="1"/>' +
                            ser(0, "Sales", sales, [8.2, 3.2, 1.4, 1.2], "", pts) +
                            f'{NO_LABELS}<c:firstSliceAng val="0"/><c:holeSize val="75"/></c:doughnutChart>', title())
    area = "".join(ser(i, n, CATS, v, fill(i), col="BC"[i]) for i, (n, v) in enumerate(SERIES[:2]))
    out["area"] = chart(f'<c:areaChart><c:grouping val="standard"/><c:varyColors val="0"/>{area}{NO_LABELS}'
                        '<c:axId val="301"/><c:axId val="302"/></c:areaChart>', title("Areas"),
                        axes=axis("catAx", 301, 302, "b") +
                        axis("valAx", 302, 301, "l", "General", True, '<c:crossBetween val="midCat"/>'))
    xs = [0.7, 1.8, 2.6, 3.1, 4.4, 5.0]
    sc = (ser(0, "Y-Values", xs, [2.7, 3.2, 0.8, 2.1, 3.9, 3.0], stroke(0, 19050), marker(0), xy=True) +
          ser(1, "Fit", xs, [1.9, 2.2, 2.4, 2.6, 2.9, 3.0], stroke(1, 19050),
              '<c:marker><c:symbol val="none"/></c:marker>', xy=True, col="C"))
    out["scatter"] = chart(f'<c:scatterChart><c:scatterStyle val="lineMarker"/><c:varyColors val="0"/>{sc}{NO_LABELS}'
                           '<c:axId val="401"/><c:axId val="402"/></c:scatterChart>', title("Scatter"),
                           axes=axis("valAx", 401, 402, "b", "General", False, VAL_AXIS) +
                           axis("valAx", 402, 401, "l", "General", True, VAL_AXIS))
    radar = "".join(ser(i, n, CATS, v, stroke(i), col="BC"[i]) for i, (n, v) in enumerate(SERIES[:2]))
    out["radar"] = chart(f'<c:radarChart><c:radarStyle val="marker"/><c:varyColors val="0"/>{radar}{NO_LABELS}'
                         '<c:axId val="501"/><c:axId val="502"/></c:radarChart>', title("Radar (a placeholder)"),
                         axes=axis("catAx", 501, 502, "b", grid=True) + axis("valAx", 502, 501, "l", "General", True, VAL_AXIS))
    return out


class ChartDoc(dt.Doc):
    def chart(self, n, xml, w=432, h=252):
        self.media[f"word/charts/chart{n}.xml"] = xml
        rid = self.rel("chart", f"charts/chart{n}.xml")
        return self.inline(w, h, CHART_URI, f'<c:chart xmlns:c="{CHART_URI}" r:id="{rid}"/>', f"Chart {n}")

    def save(self, path, header=None):
        super().save(path, header)
        # The base writes .xml parts as application/xml; charts need their own content type.
        with zipfile.ZipFile(path) as z:
            parts = [(i, z.read(i)) for i in z.infolist()]
        charts = "".join(f'<Override PartName="/{i.filename}" ContentType="{CHART_TYPE}"/>'
                         for i, _ in parts if i.filename.startswith("word/charts/"))
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
            for info, data in parts:
                if info.filename == "[Content_Types].xml":
                    data = data.replace(b"</Types>", charts.encode() + b"</Types>")
                z.writestr(info, data)


def main():
    doc = ChartDoc()
    os.makedirs(SEEDS, exist_ok=True)
    for n, (name, xml) in enumerate(charts().items(), 1):
        doc.heading(name)
        doc.para(doc.chart(n, xml))
        with open(os.path.join(SEEDS, f"{name}.xml"), "w", encoding="utf-8", newline="") as f:
            f.write(xml)
    doc.save(os.path.join(dt.GENERATED, "charts.docx"))


if __name__ == "__main__":
    main()
