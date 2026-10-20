#ifndef CALLASSIST_RESOURCE_H
#define CALLASSIST_RESOURCE_H

#define IDI_APPICON              101

#define IDD_SETTINGS             200
#define IDD_TAB_STT              201
#define IDD_TAB_LLM              202
#define IDD_TAB_AUDIO            203
#define IDD_TAB_TRIGGER          204
#define IDD_TAB_OVERLAY          205

#define IDC_TAB                  1000
#define IDC_APPLY                1001
#define IDC_STATUSBAR            1002

/* ---- speech to text ---- */
#define IDC_STT_LOCAL            1100
#define IDC_STT_REMOTE           1101
#define IDC_WMODEL               1102
#define IDC_WBROWSE              1103
#define IDC_WTHREADS             1104
#define IDC_WGPU                 1105
#define IDC_STT_URL              1106
#define IDC_STT_KEY              1107
#define IDC_STT_MODEL            1108
#define IDC_LANG                 1109
#define IDC_TRANSLATE            1110
#define IDC_STT_HINT             1111
#define IDC_MODELS_LINK          1112
#define IDC_STT_TEST             1113

/* ---- llm ---- */
#define IDC_LLM_URL              1200
#define IDC_LLM_KEY              1201
#define IDC_LLM_MODEL            1202
#define IDC_TEMP                 1203
#define IDC_MAXTOK               1204
#define IDC_CTXSEC               1205
#define IDC_STREAM               1206
#define IDC_LLM_TEST             1207
#define IDC_SYSPROMPT            1208
#define IDC_USERTPL              1209

/* ---- audio ---- */
#define IDC_CAP_MIC              1300
#define IDC_MIC_DEV              1301
#define IDC_MIC_GAIN             1302
#define IDC_CAP_SYS              1303
#define IDC_SYS_DEV              1304
#define IDC_SYS_GAIN             1305
#define IDC_VAD_TH               1306
#define IDC_VAD_TH_TXT           1307
#define IDC_VAD_SIL              1308
#define IDC_VAD_MIN              1309
#define IDC_VAD_MAX              1310
#define IDC_LEVEL                1311
#define IDC_LEVEL_TXT            1312
#define IDC_VAD_SOFT             1313
#define IDC_SRC_LEVELS           1314

/* ---- triggers ---- */
#define IDC_AUTO                 1400
#define IDC_AUTO_MIN             1401
#define IDC_AUTO_COOL            1402
#define IDC_AUTO_Q               1403
#define IDC_HK_RESPOND           1404
#define IDC_HK_OVERLAY           1405
#define IDC_HK_CLEAR             1406
#define IDC_HK_LISTEN            1407
#define IDC_START_LISTENING      1408
#define IDC_PTT                  1409
#define IDC_MOUSE_BTN            1410
#define IDC_HK_SNIP              1411
#define IDC_HK_SCROLL_UP         1412
#define IDC_HK_SCROLL_DOWN       1413
#define IDC_HK_PANE_NEXT         1414
#define IDC_TWO_STAGE            1415
#define IDC_HK_MOVE              1416

/* ---- overlay ---- */
#define IDC_OPACITY              1500
#define IDC_OPACITY_TXT          1501
#define IDC_FONTSIZE             1502
#define IDC_CLICKTHRU            1503
#define IDC_HIDECAP              1504
#define IDC_SHOWTR               1505
#define IDC_COL_BG               1506
#define IDC_COL_FG               1507
#define IDC_COL_ACCENT           1508
#define IDC_OVX                  1509
#define IDC_OVY                  1510
#define IDC_OVW                  1511
#define IDC_OVH                  1512
#define IDC_OV_RESET             1513
#define IDC_OV_GRAB              1514

/* ---- tray menu ---- */
#define IDM_SHOW_OVERLAY         2000
#define IDM_SETTINGS             2001
#define IDM_LISTEN               2002
#define IDM_RESPOND              2003
#define IDM_CLEAR                2004
#define IDM_OPEN_LOG             2005
#define IDM_SNIP                 2006
#define IDM_EXIT                 2007

#endif
