"""Seeds for fuzz_pptx (tests/fuzz/seeds/pptx): the generated decks with every XML part as text
(the format fuzz_check reads), and a deck with every structure the model reads, made from
simple.pptx. Standard library only: python tests/fuzz/make_pptx_seeds.py"""
import os
import re
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
OFFICE = os.path.join(HERE, "..", "office")
SEEDS = os.path.join(HERE, "seeds", "pptx")
REGRESSIONS = os.path.join(HERE, "regressions", "pptx")


def parts_of(path):
    with zipfile.ZipFile(path) as z:
        return {i.filename: z.read(i.filename) for i in z.infolist() if not i.filename.endswith("/")}


def as_text(parts):
    """The XML parts, without the layouts no slide uses and the parts the model doesn't read, so
    the seeds stay small enough to fuzz well."""
    used = set()
    for name, data in parts.items():
        if name.startswith("ppt/slides/_rels/"):
            used.update(m.decode() for m in re.findall(rb'Target="\.\./slideLayouts/(slideLayout\d+\.xml)"', data))
    out = []
    for name, data in parts.items():
        if not (name.endswith(".xml") or name.endswith(".rels")) or name.startswith("docProps/"):
            continue
        if any(s in name for s in ("viewProps", "printerSettings")):
            continue
        layout = re.search(r"slideLayouts/(?:_rels/)?(slideLayout\d+\.xml)", name)
        if layout and layout.group(1) not in used:
            continue
        out.append(b"@" + name.encode("utf-8") + b"\n" + data)
    return b"\n".join(out)


A = "http://schemas.openxmlformats.org/drawingml/2006/main"
P = "http://schemas.openxmlformats.org/presentationml/2006/main"
R = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
NS = f'xmlns:a="{A}" xmlns:p="{P}" xmlns:r="{R}"'


def sp(id, inner, ph="", style="", spPr=""):
    return (f'<p:sp><p:nvSpPr><p:cNvPr id="{id}" name="S{id}"><a:hlinkClick r:id="rId2"/></p:cNvPr><p:cNvSpPr/>'
            f'<p:nvPr>{ph}</p:nvPr></p:nvSpPr><p:spPr>{spPr}</p:spPr>{style}<p:txBody>{inner}</p:txBody></p:sp>')


def features():
    parts = parts_of(os.path.join(OFFICE, "generated", "simple.pptx"))
    body = ('<a:bodyPr anchor="ctr" lIns="0"><a:normAutofit fontScale="62500" lnSpcReduction="20000"/></a:bodyPr>'
            '<a:lstStyle><a:defPPr><a:defRPr sz="1400"/></a:defPPr><a:lvl2pPr marL="457200" indent="-228600">'
            '<a:buClr><a:schemeClr val="accent2"/></a:buClr><a:buSzPct val="80000"/><a:buFont typeface="+mj-lt"/>'
            '<a:buAutoNum type="romanUcPeriod" startAt="3"/><a:tabLst><a:tab pos="914400" algn="dec"/></a:tabLst>'
            '</a:lvl2pPr></a:lstStyle>'
            '<a:p><a:pPr algn="just"><a:lnSpc><a:spcPct val="90000"/></a:lnSpc><a:spcBef><a:spcPts val="600"/></a:spcBef>'
            '</a:pPr><a:r><a:rPr lang="nl-NL" sz="2400" b="1" i="1" u="dbl" strike="sngStrike" cap="small" '
            'baseline="30000" spc="100" kern="1200"><a:ln w="9525"><a:solidFill><a:srgbClr val="00FF00"/></a:solidFill>'
            '</a:ln><a:gradFill><a:gsLst><a:gs pos="0"><a:schemeClr val="accent1"><a:lumMod val="75000"/></a:schemeClr>'
            '</a:gs><a:gs pos="100000"><a:srgbClr val="FF0000"/></a:gs></a:gsLst></a:gradFill><a:highlight>'
            '<a:srgbClr val="FFFF00"/></a:highlight><a:uFill><a:solidFill><a:srgbClr val="0000FF"/></a:solidFill></a:uFill>'
            '<a:latin typeface="+mn-lt"/><a:ea typeface="+mj-ea"/><a:sym typeface="Wingdings"/>'
            '<a:hlinkClick r:id="" action="ppaction://hlinkshowjump?jump=lastslide"/></a:rPr><a:t>Run</a:t></a:r>'
            '<a:br><a:rPr sz="900"/></a:br><a:fld id="{1}" type="slidenum"><a:t>&lt;#&gt;</a:t></a:fld>'
            '<a:endParaRPr sz="3000"/></a:p>'
            '<a:p><a:pPr lvl="1"/><a:r><a:t>numbered</a:t></a:r></a:p><a:p><a:pPr lvl="1"/><a:r><a:t>again</a:t></a:r></a:p>'
            '<a:p><a:pPr lvl="9"><a:buBlip><a:blip r:embed="rId9"/></a:buBlip></a:pPr><a:r><a:t>deep</a:t></a:r></a:p>')
    style = ('<p:style><a:lnRef idx="2"><a:schemeClr val="accent1"/></a:lnRef><a:fillRef idx="1001"><a:schemeClr '
             'val="bg2"/></a:fillRef><a:effectRef idx="0"><a:schemeClr val="accent1"/></a:effectRef><a:fontRef '
             'idx="major"><a:schemeClr val="lt1"/></a:fontRef></p:style>')
    table = ('<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id="9" name="T"/><p:cNvGraphicFramePr/><p:nvPr/>'
             '</p:nvGraphicFramePr><p:xfrm><a:off x="0" y="0"/><a:ext cx="2540000" cy="1270000"/></p:xfrm><a:graphic>'
             '<a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/table"><a:tbl><a:tblPr firstRow="1" '
             'lastRow="1" firstCol="1" lastCol="1" bandRow="1" bandCol="1"><a:tableStyleId>'
             '{5C22544A-7EE6-4342-B048-85BDC9FD1C3A}</a:tableStyleId></a:tblPr><a:tblGrid><a:gridCol w="846666"/>'
             '<a:gridCol w="846666"/><a:gridCol w="846666"/></a:tblGrid>'
             + "".join('<a:tr h="317500">' + "".join(
                 f'<a:tc{" gridSpan=" + chr(34) + "2" + chr(34) if (r, c) == (1, 0) else ""}'
                 f'{" hMerge=" + chr(34) + "1" + chr(34) if (r, c) == (1, 1) else ""}'
                 f'{" rowSpan=" + chr(34) + "2" + chr(34) if (r, c) == (2, 2) else ""}'
                 f'{" vMerge=" + chr(34) + "1" + chr(34) if (r, c) == (3, 2) else ""}>'
                 f'<a:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>{r}{c}</a:t></a:r></a:p></a:txBody>'
                 '<a:tcPr marL="0" anchor="b" vert="vert270"><a:lnL w="25400"><a:solidFill><a:srgbClr val="FF0000"/>'
                 '</a:solidFill><a:prstDash val="sysDot"/></a:lnL><a:lnTlToBr><a:noFill/></a:lnTlToBr><a:solidFill>'
                 '<a:schemeClr val="accent3"/></a:solidFill></a:tcPr></a:tc>' for c in range(3)) + "</a:tr>"
                 for r in range(4))
             + '</a:tbl></a:graphicData></a:graphic></p:graphicFrame>')
    group = ('<p:grpSp><p:nvGrpSpPr><p:cNvPr id="10" name="G"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr>'
             '<a:xfrm rot="5400000" flipH="1"><a:off x="100" y="100"/><a:ext cx="1000" cy="1000"/><a:chOff x="0" y="0"/>'
             '<a:chExt cx="2000" cy="2000"/></a:xfrm></p:grpSpPr>' + sp(11, body) +
             '<p:pic><p:nvPicPr><p:cNvPr id="12" name="Movie"/><p:cNvPicPr/><p:nvPr><a:videoFile r:link="rId2"/>'
             '</p:nvPr></p:nvPicPr><p:blipFill><a:blip r:embed="rId9"/></p:blipFill><p:spPr/></p:pic>'
             '<mc:AlternateContent xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006">'
             '<mc:Choice xmlns:p14="http://schemas.microsoft.com/office/powerpoint/2010/main" Requires="p14">'
             '<p:contentPart r:id="rId9"/></mc:Choice><mc:Fallback>' + sp(13, body) + '</mc:Fallback></mc:AlternateContent>'
             '</p:grpSp>')
    shapes = (sp(2, body, '<p:ph type="ctrTitle"/>') + sp(3, body, '<p:ph type="subTitle" idx="1"/>') +
              sp(4, body, '<p:ph type="dt" sz="half" idx="10"/>') + sp(5, body, '<p:ph type="pic" idx="99"/>') +
              sp(6, body, "", style, '<a:xfrm><a:off x="1" y="2"/><a:ext cx="3" cy="4"/></a:xfrm>'
                 '<a:prstGeom prst="wedgeRoundRectCallout"><a:avLst/></a:prstGeom>') + table + group)
    slide = (f'<p:sld {NS} show="0"><p:cSld><p:bg><p:bgPr><a:gradFill><a:gsLst><a:gs pos="0"><a:schemeClr val="bg1"/>'
             '</a:gs><a:gs pos="100000"><a:schemeClr val="accent1"/></a:gs></a:gsLst></a:gradFill><a:effectLst/></p:bgPr>'
             '</p:bg><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>'
             + shapes + '</p:spTree></p:cSld><p:clrMapOvr><a:overrideClrMapping bg1="dk1" tx1="lt1" bg2="dk2" '
             'tx2="lt2" accent1="accent1" accent2="accent2" accent3="accent3" accent4="accent4" accent5="accent5" '
             'accent6="accent6" hlink="hlink" folHlink="folHlink"/></p:clrMapOvr>'
             '<p:transition spd="slow" advTm="3000"><p:push dir="u"/></p:transition><p:timing><p:tnLst><p:par>'
             '<p:cTn id="1" dur="indefinite" restart="never" nodeType="tmRoot"/></p:par></p:tnLst></p:timing></p:sld>')
    parts["ppt/slides/slide1.xml"] = slide.encode("utf-8")
    parts["ppt/slides/_rels/slide1.xml.rels"] = (
        '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
        f'<Relationship Id="rId1" Type="{R}/slideLayout" Target="../slideLayouts/slideLayout1.xml"/>'
        f'<Relationship Id="rId2" Type="{R}/slide" Target="slide2.xml"/>'
        f'<Relationship Id="rId9" Type="{R}/image" Target="../media/none.png"/></Relationships>').encode("utf-8")
    pres = parts["ppt/presentation.xml"].decode("utf-8")
    extra = ('<p:custShowLst><p:custShow name="Short" id="0"><p:sldLst><p:sld r:id="rId8"/></p:sldLst></p:custShow>'
             '</p:custShowLst><p:extLst><p:ext uri="{521415D9-36F7-43E2-AB2F-B90AF26B5E84}"><p14:sectionLst '
             'xmlns:p14="http://schemas.microsoft.com/office/powerpoint/2010/main"><p14:section name="One" id="{1}">'
             '<p14:sldIdLst><p14:sldId id="256"/><p14:sldId id="999"/></p14:sldIdLst></p14:section></p14:sectionLst>'
             '</p:ext></p:extLst>')
    pres = pres.replace("</p:defaultTextStyle>", "</p:defaultTextStyle>" + extra)
    pres = pres.replace("<p:sldSz", '<p:embeddedFontLst><p:embeddedFont><p:font typeface="Embedded"/>'
                        '<p:regular r:id="rId7"/></p:embeddedFont></p:embeddedFontLst><p:sldSz')
    pres = pres.replace("<p:presentation ", '<p:presentation firstSlideNum="5" ')
    parts["ppt/presentation.xml"] = pres.encode("utf-8")
    return parts


def cycle():
    """Regression-style: a layout whose master is itself a layout, and a slide that is its own layout."""
    parts = parts_of(os.path.join(OFFICE, "generated", "simple.pptx"))
    parts["ppt/slides/_rels/slide1.xml.rels"] = (
        '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
        f'<Relationship Id="rId1" Type="{R}/slideLayout" Target="slide1.xml"/></Relationships>').encode("utf-8")
    parts["ppt/slideLayouts/_rels/slideLayout2.xml.rels"] = (
        '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
        f'<Relationship Id="rId1" Type="{R}/slideMaster" Target="slideLayout2.xml"/></Relationships>').encode("utf-8")
    return parts


def write(folder, name, data):
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, name), "wb") as f:
        f.write(data)


def main():
    for deck in ("simple", "layouts", "shapes-table-chart"):
        write(SEEDS, f"generated-{deck}.txt", as_text(parts_of(os.path.join(OFFICE, "generated", deck + ".pptx"))))
    write(SEEDS, "features.txt", as_text(features()))
    write(REGRESSIONS, "cycles.txt", as_text(cycle()))


if __name__ == "__main__":
    main()
