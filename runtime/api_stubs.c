/* 其余杂项导入（comdlg32 等） */
#include "host.h"

/* 文件对话框：原版只在开发调试时用到，运行时直接返回“用户取消” */
API_FN(GetOpenFileNameA) { HOST_RET(0, 1); }
API_FN(GetSaveFileNameA) { HOST_RET(0, 1); }

void api_register_misc(void) {
    static const ApiEntry t[] = { API_ENTRY(GetOpenFileNameA), API_ENTRY(GetSaveFileNameA) };
    api_register(t, 2);
}
