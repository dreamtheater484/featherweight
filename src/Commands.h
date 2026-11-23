#pragma once

// Every user action has one command id, shared by buttons, menus and keyboard shortcuts.
enum Cmd : int {
  CmdNone = 0,
  CmdOpen = 100,
  CmdSave,
  CmdSaveAs,
  CmdPrint,
  CmdCloseTab,
  CmdNextTab,
  CmdPrevTab,
  CmdFind,
  CmdFindNext,
  CmdFindPrev,
  CmdCloseFind,
  CmdCopy,
  CmdSelectAll,
  CmdSelectAllOnPage,
  CmdGoToPage,
  CmdNextPage,
  CmdPrevPage,
  CmdFirstPage,
  CmdLastPage,
  CmdZoomIn,
  CmdZoomOut,
  CmdZoomMenu,
  CmdZoomAuto,
  CmdZoomActual,
  CmdZoomFitPage,
  CmdZoomFitWidth,
  CmdViewMenu,
  CmdViewSingle,
  CmdViewContinuous,
  CmdViewFacing,
  CmdToggleCover,
  CmdRotateLeft,
  CmdRotateRight,
  CmdToggleInvert,
  CmdFullScreen,
  CmdPresentation,
  CmdMoreMenu,
  CmdThemeSystem,
  CmdThemeLight,
  CmdThemeDark,
  CmdToggleHighlight,
  CmdToggleAutoUpdate,
  CmdCheckUpdates,
  CmdMakeDefault,
  CmdAbout,
  CmdClearRecent,
  CmdExit,
  CmdNewTab,
  CmdSearchCase,
  CmdSaveMenu,
  CmdHighlight,
  CmdAddNote,
  CmdOpenNote,
  CmdRemoveAnnot,
  CmdCut,
  CmdPaste,
  CmdCommandSearch,
  CmdUndo,
  CmdRedo,
  CmdReplace,     // find and replace (Ctrl+H)
  CmdReplaceOne,
  CmdReplaceAll,
  CmdRestoreOriginal,
  CmdDeleteBackups,
  CmdZoomPreset = 400,  // + index into fw::ZoomPresets()
  CmdRecent = 500,      // + index into Settings::recent
  CmdBannerAction = 600,
  CmdNewFile = 700,     // + fw::FileKind, once that kind's editor ships
  CmdNewTemplate = 720, // + index into EmptyState::kTemplates (D5): a new document from a template
  // s4-ods (3020-3039): the user's own templates (UserTemplates.h).
  CmdUserTemplate = 3020,         // + index (up to kUserTemplateCount): a new document or workbook from one
  CmdUserTemplatesFolder = 3036,  // "More templates...": the Open dialog in the personal templates folder
  // p1-show (3300-3349): the slide show (src/slide/show/SlideShow.h).
  CmdSlideShowFromStart = 3300,    // F5
  CmdSlideShowFromCurrent = 3301,  // Shift+F5
  CmdSlideShowMonitor = 3310,      // + index into fw::slide::show::Monitors(): where the show goes

  // Documents: formatting (D2). The toolbar's boxes have ids too.
  CmdBold = 1000,
  CmdItalic,
  CmdUnderline,
  CmdStrike,
  CmdSubscript,
  CmdSuperscript,
  CmdGrowFont,
  CmdShrinkFont,
  CmdGrowFontPoint,
  CmdShrinkFontPoint,
  CmdClearCharFormat,
  CmdClearParaFormat,
  CmdClearAllFormat,
  CmdFontColor,
  CmdHighlightColor,
  CmdFormatPainter,
  CmdCopyFormat,
  CmdPasteFormat,
  CmdAlignLeft,
  CmdAlignCenter,
  CmdAlignRight,
  CmdAlignJustify,
  CmdAlignBox,
  CmdSpacing1,
  CmdSpacing15,
  CmdSpacing2,
  CmdSpaceBefore,
  CmdBullets,
  CmdNumbering,
  CmdMultilevel,
  CmdRestartNumbering,
  CmdContinueNumbering,
  CmdIndent,
  CmdOutdent,
  CmdHangingIndent,
  CmdUnhangIndent,
  CmdStyleNormal,
  CmdStyleHeading1,
  CmdStyleHeading2,
  CmdStyleHeading3,
  CmdUpdateStyle,
  CmdStyleBox,
  CmdFontBox,
  CmdSizeBox,
  CmdInsertBox,
  CmdInsertDate,
  CmdInsertTime,
  CmdSpacing115,
  CmdBorderBox,
  CmdBorderBottom,
  CmdBorderNone,
  CmdShading,
  CmdShadingNone,
  // Headers and footers, page numbers, links, bookmarks, navigation and marks (D3).
  CmdEditHeader = 1100,
  CmdEditFooter,
  CmdCloseHeaderFooter,
  CmdDifferentFirstPage,
  CmdDifferentOddEven,
  CmdLinkToPrevious,
  CmdPageNumber,  // the page number panel
  CmdInsertLink,  // Ctrl+K: insert or edit
  CmdRemoveLink,
  CmdOpenLink,
  CmdBookmark,
  CmdNavigationPane,
  CmdShowMarks,  // Ctrl+Shift+8
  // Documents: tables (D3).
  CmdTableGrid = 1150,  // the toolbar's Insert table grid (after the header and footer commands)
  CmdTableBox,          // the toolbar's Table menu, in a table
  CmdInsertTable,       // the Table panel, to insert a table of any size
  CmdTextToTable,
  CmdTableToText,
  CmdTableInsertAbove,
  CmdTableInsertBelow,
  CmdTableInsertLeft,
  CmdTableInsertRight,
  CmdTableDeleteRows,
  CmdTableDeleteColumns,
  CmdTableDeleteTable,
  CmdTableMerge,
  CmdTableSplit,
  CmdTableSelectCell,
  CmdTableSelectRow,
  CmdTableSelectColumn,
  CmdTableSelectTable,
  CmdTableAutoFitContents,
  CmdTableAutoFitWindow,
  CmdTableFixedWidth,
  CmdTableDistributeColumns,
  CmdTableHeaderRow,
  CmdTableProperties,
  // Documents: page layout (D3).
  CmdPageSetup = 1250,  // the Page setup panel
  CmdLayoutBox,         // the toolbar's Layout menu
  CmdPortrait,
  CmdLandscape,
  CmdMarginsNormal,     // Word's presets
  CmdMarginsNarrow,
  CmdMarginsModerate,
  CmdMarginsWide,
  CmdColumnsOne,
  CmdColumnsTwo,
  CmdColumnsThree,
  CmdPageBreak,         // Ctrl+Enter
  CmdColumnBreak,       // Ctrl+Shift+Enter
  CmdSectionNextPage,
  CmdSectionContinuous,
  CmdSectionEvenPage,
  CmdSectionOddPage,
  CmdDeleteSectionBreak,
  CmdHyphenation,       // automatic hyphenation on or off
  CmdOptionalHyphen,    // Ctrl+-
  CmdNonBreakingHyphen, // Ctrl+Shift+-
  CmdParagraph,         // the Paragraph panel (indents, spacing, line and page breaks)
  // Proofing in Word documents.
  CmdSpelling = 800,  // F7: the next spelling error
  CmdWordCount,
  CmdLanguage,        // the selection's language, from a menu
  CmdToggleSpelling,
  CmdToggleAutoCorrect,
  CmdSpellIgnore,
  CmdSpellAdd,
  CmdSpellDelete,           // a repeated word
  CmdSpellSuggest = 820,    // + index into the context menu's suggestions
  CmdLanguagePick = 840,    // + index into the installed languages
  CmdLanguageLast = 899,
  // Pictures in Word documents (D3).
  CmdInsertPicture = 900,
  CmdPictureInline,
  CmdPictureSquare,
  CmdPictureTight,
  CmdPictureTopBottom,
  CmdPictureBehind,
  CmdPictureInFront,
  CmdPictureWrapBox,  // the toolbar's Wrap text box
  CmdPictureCrop,     // crop mode on or off
  CmdPictureRotate,   // 90 degrees clockwise
  CmdPictureReset,
  CmdPicturePanel,    // size, rotation and alt text
  // Shapes, text boxes and watermarks in Word documents (D4): 1460-1479, as claude/d4-int has them.
  CmdInsertTextBox = 1460,
  CmdInsertRectangle,
  CmdInsertRoundedRectangle,
  CmdInsertOval,
  CmdInsertLine,
  CmdInsertArrow,
  CmdInsertCallout,
  CmdShapeAddText,     // an empty paragraph in a shape without text
  CmdShapeEditText,    // the caret into the selected shape's text
  CmdShapePanel,       // wrap, size, rotation, fill and outline
  CmdWatermarkPanel,
  CmdWatermarkRemove,
  // Find and paste (D3).
  CmdSearchWords = 1300,  // find whole words only (the search card's toggle)
  CmdPasteMerge,          // paste taking the look of where it goes, keeping bold, italic and lists
  // Review (D4): comments and tracked changes in Word documents.
  CmdReviewPane = 1320,  // the toolbar's 💬: the Review pane
  CmdNewComment,         // Ctrl+Alt+M: a comment on the selection
  CmdTrackChanges,       // Ctrl+Shift+E: Track Changes on or off
  CmdAcceptChange,       // the change at the caret (or the selection's), then the next
  CmdRejectChange,
  CmdAcceptAll,
  CmdRejectAll,
  CmdNextChange,
  CmdPrevChange,
  CmdMarkupSimple,  // what the changes look like: Word's Simple Markup, All Markup, No Markup, Original
  CmdMarkupAll,
  CmdMarkupNone,
  CmdMarkupOriginal,
  CmdReviewerName,  // ⋯: the name changes and comments are made with
  // Footnotes, endnotes and tables of contents (D4).
  CmdInsertFootnote = 1340,  // Ctrl+Alt+F
  CmdInsertEndnote,          // Ctrl+Alt+D
  CmdInsertToc,
  CmdUpdateTocPages,  // the table of contents' page numbers
  CmdUpdateToc,       // the whole table of contents
  // The ruler in Word documents (D3).
  CmdRuler = 1200,  // the rulers shown or not (the ⋯ menu, a setting like spelling as you type)
  // Change case (D3): Shift+F3 and the Aa menu.
  CmdChangeCase,  // Shift+F3: the next case in Word's cycle
  CmdCaseSentence,
  CmdCaseLower,
  CmdCaseUpper,
  CmdCaseCapitalize,
  CmdCaseToggle,
  CmdCaseBox,  // the toolbar's Aa menu
  // Tab stops (D3): the ruler's menu on a tab stop, and the Tabs panel.
  CmdTabsPanel = 1210,
  CmdTabLeft,
  CmdTabCenter,
  CmdTabRight,
  CmdTabDecimal,
  CmdTabBar,
  CmdTabLeaderNone,
  CmdTabLeaderDots,
  CmdTabLeaderDashes,
  CmdTabLeaderLine,
  CmdTabClear,
  CmdTabClearAll,
  // Styles (D3).
  CmdModifyStyle = 1230,  // the Modify style panel, for the paragraph's style
  CmdNewStyle,            // Create a style from the selection
  // Output (D4).
  CmdExportPdf = 1400,  // File > Export PDF: the tab's pages into a PDF file
  // Content controls (D4): Insert ▾ adds them, as Word's Developer tab does.
  CmdInsertRichText = 1410,
  CmdInsertPlainText,
  CmdInsertCheckBox,
  CmdInsertDropDown,
  CmdInsertDatePicker,
  // Spreadsheets (S1).
  CmdPrintPreview = 1500,  // the workbook as it prints, page by page (and back to the grid)
  CmdNextSheet,            // Ctrl+PgDn
  CmdPrevSheet,            // Ctrl+PgUp
  CmdSheetPick = 1520,     // + the sheet's index: the sheet tabs' list of every sheet
  CmdSheetPickLast = 1999,
  // Editing spreadsheets (S2, the grid): 2000-2699.
  CmdInsertCells = 2000,  // Ctrl++: cells, or whole rows or columns when they're selected
  CmdDeleteCells,         // Ctrl+-
  CmdClearContents,       // Delete
  CmdRowHeight,           // Row height…
  CmdColumnWidth,         // Column width…
  CmdHide,                // the selected rows or columns
  CmdUnhide,
  CmdInsertSheet,         // Shift+F11
  CmdDeleteSheet,
  CmdRenameSheet,
  CmdHideSheet,
  CmdMoveSheetLeft,
  CmdMoveSheetRight,
  CmdGoTo,                // Ctrl+G, F5
  CmdFillDown,            // Ctrl+D
  CmdFillRight,           // Ctrl+R
  CmdPasteValues,         // Ctrl+Shift+V
  CmdPasteFormulas,
  CmdPasteFormats,
  CmdPasteTranspose,
  CmdAutoSum,             // Alt+=
  CmdCalculateNow,        // F9
  CmdCalculateSheet,      // Shift+F9
  CmdCalculateFull,       // Ctrl+Alt+F9
  CmdMergeCenter = 2024,  // Merge & Center
  CmdMergeAcross,         // each selected row merged on its own
  CmdMergeCells,
  CmdUnmerge,
  CmdFreezePanes,         // at the active cell; Unfreeze panes when frozen
  CmdFreezeTopRow,
  CmdFreezeFirstColumn,
  CmdGroup,               // Alt+Shift+Right: the selected rows, or columns
  CmdUngroup,             // Alt+Shift+Left
  CmdAutoFitRowHeight,
  CmdAutoFitColumnWidth,  // as double-clicking the header's edge
  CmdInsertShiftRight,    // Excel's Insert choices for cells that aren't whole rows or columns
  CmdInsertShiftDown,
  CmdInsertRows,
  CmdInsertColumns,
  CmdDeleteShiftLeft,     // and Delete's
  CmdDeleteShiftUp,
  CmdDeleteRows,
  CmdDeleteColumns,
  CmdTabColor = 2100,     // + index into the tab colors (0: none)
  CmdUnhideSheet = 2200,  // + the sheet's index: a hidden sheet shown again
  CmdUnhideSheetLast = 2699,
  // Formats, sort and filter (S2, s2-format): 2700-2999.
  CmdFormatCells = 2700,  // Ctrl+1
  CmdNumberFormatBox,     // the toolbar's number formats
  CmdIncreaseDecimal,
  CmdDecreaseDecimal,
  CmdMergeBox,            // the toolbar's merge choices (s2-grid's merge commands)
  CmdBordersBox,          // the toolbar's borders; its main part repeats the last choice
  CmdFillColor,
  CmdStrikethrough,       // Ctrl+5
  CmdNumGeneral,          // Ctrl+Shift+~
  CmdNumNumber,           // Ctrl+Shift+1
  CmdNumTime,             // Ctrl+Shift+2
  CmdNumDate,             // Ctrl+Shift+3
  CmdNumCurrency,         // Ctrl+Shift+4
  CmdNumPercent,          // Ctrl+Shift+5
  CmdNumScientific,       // Ctrl+Shift+6
  CmdBorderOutline,       // Ctrl+Shift+7
  CmdBorderRemove,        // Ctrl+Shift+-
  CmdCellStyles,          // the toolbar's built-in cell styles
  CmdFilter,              // Ctrl+Shift+L: filter on or off
  CmdDataBox,             // the toolbar's sort and filter choices
  CmdSortAscending,
  CmdSortDescending,
  CmdRemoveDuplicates,
  // Charts and the objects in a sheet's drawing (S3, s3-charts): 2800-2849.
  CmdInsertChart = 2800,  // Alt+F1: the default chart (clustered column) of the selection
  CmdChartColumn,         // Insert > Chart by type, in ChartKind's order
  CmdChartBar,
  CmdChartLine,
  CmdChartPie,
  CmdChartDoughnut,
  CmdChartArea,
  CmdChartScatter,
  CmdChartBox,            // the toolbar's chart choices
  CmdDeleteObject,        // the selected chart, picture or shape
  CmdChartPanel,          // the selected chart's panel (double-click, Ctrl+1)
  CmdMoveChart,           // Move Chart > New sheet: the selected chart on a chart sheet of its own;
                          // on a chart sheet, its chart back onto the first worksheet
  CmdChartCombo,          // Insert > Chart > Combo (clustered column and line)
  // Conditional formatting and data validation (S3, s3-data): 2850-2899.
  CmdCondFormat = 2850,   // the Conditional formatting panel: a new rule
  CmdCfManage,            // its list of the sheet's rules
  CmdCfClearSelection,
  CmdCfClearSheet,
  CmdCondFormatBox,       // the toolbar's conditional formatting choices
  CmdDataValidation,      // Data > Data validation
  CmdCircleInvalid,
  CmdClearCircles,
  CmdValidationList,      // Alt+Down on a cell with a list: its choices
  CmdCreateTable,         // Ctrl+T, Ctrl+L
  CmdTableDesign,
  CmdConvertToRange,
  CmdTableTotalRow,
  // Pictures and shapes (S3, s3-data): 2880-2899.
  CmdObjectsBox = 2880,   // the toolbar's Insert choices
  CmdObjectPicture,       // Insert > Pictures: files
  CmdObjectRectangle,     // Insert > Shapes, at the active cell
  CmdObjectRoundRect,
  CmdObjectOval,
  CmdObjectLine,
  CmdObjectArrow,
  CmdObjectTextBox,
  CmdFormatObject,        // the Format panel of the selected picture or shape
  // Protection, links, notes and comments (S3, s3-review): 2900-2939.
  CmdProtectSheet = 2900,  // Protect Sheet, or Unprotect Sheet on a protected one
  CmdProtectWorkbook,      // Protect Workbook (its structure), or Unprotect Workbook
  CmdNewNote,              // Shift+F2: New Note, or Edit Note on a cell with one
  CmdDeleteNote,
  CmdShowNote,             // Show/Hide Note
  CmdShowAllNotes,
  CmdDeleteThread,  // Delete Thread (New Comment, Ctrl+Alt+M, is the app's CmdNewComment)
  // Links: the app's CmdInsertLink (Ctrl+K) and CmdRemoveLink.

  // Page setup, print preview and Export PDF (S3, s3-print): 2940-2969.
  // The Page Setup panel is the app's CmdPageSetup.
  CmdSetPrintArea = 2940,  // the selection
  CmdAddToPrintArea,
  CmdClearPrintArea,
  CmdPrintTitles,       // the panel's Sheet tab
  CmdInsertPageBreak,   // above and left of the active cell
  CmdRemovePageBreak,
  CmdResetPageBreaks,
  CmdPageBreakPreview,  // the sheet's pages over its cells (on and off)
  CmdNoScaling,         // print preview's scaling shortcuts, in this order
  CmdFitSheetOnePage,
  CmdFitColumnsOnePage,
  CmdFitRowsOnePage,
  CmdPrintActiveSheet,  // what print preview shows, prints and exports, in this order
  CmdPrintWorkbook,
  CmdPrintSelection,

  // Defined names (S3, s3-calc): 2970-2999.
  CmdNameManager = 2970,  // Ctrl+F3
  CmdDefineName,
  CmdCreateNames,         // Ctrl+Shift+F3
  CmdPasteNames,          // F3

  // The ribbon (R1, r1-control): 3600-3649.
  CmdRibbonCollapse = 3600,  // Ctrl+F1: the ribbon shows only its tabs, or all of it again
  CmdFilePage,               // the File tab: the File page (New, Open, Save, Print, options)
  // Pivot tables (S4, s4-pivot): 3040-3079.
  CmdPivotRefresh = 3040,  // Alt+F5: the PivotTable at the active cell
  CmdPivotRefreshAll,      // Ctrl+Alt+F5
  CmdPivotTable,           // Insert > PivotTable: its panel

  // The workbook's ribbon (R1, r1-sheet): 3700-3749. What Excel's buttons do that Featherweight
  // did only in Format Cells or the Page Setup panel, and the ribbon's menus (their lists come
  // from WorkbookView::FillMenu or the item's `menu`).
  CmdFormatCellsFont = 3700,  // Home > Font's launcher: Format Cells on its Font page
  CmdFormatCellsAlignment,    // Home > Alignment's launcher
  CmdAlignTop,                // the cells' vertical alignment (a second click: bottom again)
  CmdAlignMiddle,
  CmdAlignBottom,
  CmdWrapText,                // on or off
  CmdNumAccounting,           // Accounting Number Format
  CmdCommaStyle,              // the Comma cell style
  CmdViewNormal,              // View > Normal: page break preview and print preview off
  CmdCellsFormatMenu = 3720,  // Home > Cells > Format ▾
  CmdFillMenu,                // Home > Editing > Fill ▾
  CmdClearMenu,               // Clear ▾
  CmdSortFilterMenu,          // Sort & Filter ▾
  CmdFindSelectMenu,          // Find & Select ▾
  CmdShapesMenu,              // Insert > Shapes ▾
  CmdColumnChartMenu,         // Insert > Charts: Column or Bar ▾, Line or Area ▾, Pie or Doughnut ▾
  CmdLineChartMenu,
  CmdPieChartMenu,
  CmdMarginsMenu,             // Page Layout: Margins ▾, Orientation ▾, Print Area ▾, Breaks ▾
  CmdOrientationMenu,
  CmdPrintAreaMenu,
  CmdBreaksMenu,
  CmdScaleWidth,              // Scale to Fit: Width ▾ and Height ▾ (Automatic or 1 page)
  CmdScaleHeight,
  CmdNotesMenu,               // Review > Notes ▾
  CmdFreezeMenu,              // View > Freeze Panes ▾
  CmdPrintWhatMenu,           // Print Preview: what prints ▾, Scaling ▾
  CmdScalingMenu,

  // The toolbar in a narrow window (S4, s4-friction): 3100-3119.
  CmdToolbarOverflow = 3100,  // the menu of the items that don't fit

  // Presentations (P1, p1-view): 3240-3299. F5 and Shift+F5 are p1-show's CmdSlideShowFromStart and
  // CmdSlideShowFromCurrent (the window's Present runs the first); the slides use CmdNextPage,
  // CmdPrevPage, CmdFirstPage, CmdLastPage and the zoom commands.
  CmdNotesPane = 3240,           // the speaker notes below the slide, shown or hidden
  CmdCopySlideText,
  CmdCopySlidePicture,
  CmdCopyNotes,
  CmdOpenSlideLink,              // the link under the right-click
  CmdPrintLayoutMenu,            // the toolbar's Print layout ▾
  CmdPrintSlides,                // what Print and Export PDF lay out: slides,
  CmdPrintHandouts1,             // handouts with 1, 2, 3, 4, 6 or 9 slides a page (in that order),
  CmdPrintHandouts2,
  CmdPrintHandouts3,
  CmdPrintHandouts4,
  CmdPrintHandouts6,
  CmdPrintHandouts9,
  CmdPrintNotes,                 // or notes pages
  CmdPrintHidden,                // hidden slides print too
  CmdPrintFrame,                 // a frame around the slides on handouts and notes pages

  // Slides in the presentation tab (P2, p2-slides): 3420-3459. Cut, copy, paste, delete and select
  // all in the thumbnails use CmdCut, CmdCopy, CmdPaste, CmdSlideDelete and CmdSelectAll.
  CmdSlideNew = 3420,            // Ctrl+M: after the current slide, on the layout the gallery shows first
  CmdSlideNewMenu,               // the toolbar's ＋ Slide ▾: the layout gallery
  CmdSlideDuplicate,             // Ctrl+D: the selected slides, after the last of them
  CmdSlideDelete,                // Delete in the thumbnails
  CmdSlideHide,                  // the selected slides skipped by the show, or shown again
  CmdSlideLayoutMenu,            // Layout ▾: the selected slides' layout
  CmdSlideMoveUp,                // Ctrl+Up in the thumbnails
  CmdSlideMoveDown,              // Ctrl+Down
  CmdSlideMoveFirst,             // Ctrl+Shift+Up
  CmdSlideMoveLast,              // Ctrl+Shift+Down
  CmdSlideSizePanel,             // Design > Slide size: 16:9 or 4:3, and how the content scales
  CmdSlideSize169,               // the slide size, directly
  CmdSlideSize43,
  CmdDesignPanel,                // Design: themes and theme colors
  CmdFormatBackground,           // Format background: solid, gradient, picture; this slide or all
  CmdResetBackground,            // the selected slides' backgrounds back to their layout's
  CmdHeaderFooter,               // Insert > Header & footer: date, slide number, footer
  CmdPasteSlidesDestination,     // paste copied slides with this presentation's design (Ctrl+V's)
  CmdPasteSlidesSource,          // paste copied slides keeping their own design
  CmdNotesFocus,                 // the keyboard into the speaker notes
  CmdSlideLayoutPick = 3440,     // + index into the current slide's master's layouts (up to 20): Layout ▸
  // Objects on slides (P2, p2-objects): 3500-3549. Inserting also uses the shared
  // CmdInsertPicture, CmdInsertTextBox, CmdInsertRectangle... CmdInsertCallout, CmdInsertTable,
  // CmdInsertChart and CmdChartColumn/Bar/Line/Pie; CmdDeleteObject, CmdPictureCrop, CmdShapePanel.
  CmdInsertShape = 3500,         // the toolbar's Shapes ▾ (its list inserts one)
  CmdGroupObjects,               // Ctrl+G
  CmdUngroupObjects,             // Ctrl+Shift+G
  CmdBringToFront,               // Ctrl+Shift+]
  CmdBringForward,               // Ctrl+]
  CmdSendBackward,               // Ctrl+[
  CmdSendToBack,                 // Ctrl+Shift+[
  CmdObjAlignLeft,               // align, in this order: left, center, right, top, middle, bottom
  CmdObjAlignCenter,
  CmdObjAlignRight,
  CmdObjAlignTop,
  CmdObjAlignMiddle,
  CmdObjAlignBottom,
  CmdDistributeHorizontally,
  CmdDistributeVertically,
  CmdAlignToSlide,               // align and distribute to the slide, not the selection
  CmdArrangeMenu,                // the toolbar's Arrange ▾
  CmdRotateRight90,
  CmdRotateLeft90,
  CmdFlipHorizontal,
  CmdFlipVertical,
  CmdObjectFill,                 // the toolbar's fill color
  CmdObjectOutline,              // outline color
  CmdObjectOutlineWeight,
  CmdObjectShadow,               // a shadow, on or off
  CmdTableRowAbove,
  CmdTableRowBelow,
  CmdTableColumnLeft,
  CmdTableColumnRight,
  CmdTableDeleteRow,
  CmdTableDeleteColumn,
  CmdDuplicateObjects,           // Ctrl+D with objects selected
  CmdTableMergeCells,            // the cells chosen in a table (text editing's) become one
  CmdTableSplitCell,             // a merged cell back into its cells
  CmdTableLayoutMenu,            // the toolbar's Layout dropdown with a table selected
};
