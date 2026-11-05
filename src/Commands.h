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
  // Find and paste (D3).
  CmdSearchWords = 1300,  // find whole words only (the search card's toggle)
  CmdPasteMerge,          // paste taking the look of where it goes, keeping bold, italic and lists
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
  // Content controls (D4): Insert ▾ adds them, as Word's Developer tab does.
  CmdInsertRichText = 1400,
  CmdInsertPlainText,
  CmdInsertCheckBox,
  CmdInsertDropDown,
  CmdInsertDatePicker,
};
