package wang.zaiyi.flyingsb;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.Charset;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** 启动页（竖屏）：选择含游戏的 ZIP 或目录，导入到应用外部私有目录后进入游戏（横屏）。 */
public class LauncherActivity extends Activity {
    private static final int REQ_ZIP = 1, REQ_DIR = 2;

    private TextView status;
    private ProgressBar busy;
    private Button btnStart, btnZip, btnDir;
    private volatile boolean importing;
    private boolean started;
    private long copiedBytes;
    private int copiedFiles;
    private long lastUi;

    /** 应用外部私有目录 Android/data/<包名>/files/：文件管理器和电脑可见；游戏数据放在其下的 game/，和原生端一致 */
    private File baseDir() {
        File d = getExternalFilesDir(null);
        return d != null ? d : getFilesDir();
    }

    private File gameDir() { return new File(baseDir(), "game"); }

    /** 游戏目录里有 FlyingSB.exe（不区分大小写）才算装好了 */
    private static boolean hasExe(File dir) {
        String[] names = dir.list();
        if (names == null) return false;
        for (String n : names) if (n.equalsIgnoreCase("FlyingSB.exe")) return true;
        return false;
    }

    private int dp(int v) { return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()); }

    private Button button(String text, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(text);
        b.setTextSize(16);
        b.setOnClickListener(l);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(52));
        lp.topMargin = dp(12);
        b.setLayoutParams(lp);
        return b;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        int pad = dp(24);
        root.setPadding(pad, pad, pad, pad);
        // 目标 SDK 35 起界面会延伸到系统栏下面，按系统栏高度留出空隙
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            v.setPadding(pad + insets.getSystemWindowInsetLeft(), pad + insets.getSystemWindowInsetTop(),
                         pad + insets.getSystemWindowInsetRight(), pad + insets.getSystemWindowInsetBottom());
            return insets;
        });

        TextView title = new TextView(this);
        title.setText("幻想西游记");
        title.setTextSize(28);
        title.setGravity(Gravity.CENTER);
        title.setPadding(0, dp(48), 0, dp(8));
        root.addView(title);

        TextView hint = new TextView(this);
        hint.setText("游戏数据不包含在本应用内。请选择你自己拥有的原版游戏：含 FlyingSB.exe 的 ZIP 压缩包或目录。");
        hint.setTextSize(14);
        hint.setGravity(Gravity.CENTER);
        hint.setPadding(0, 0, 0, dp(24));
        root.addView(hint);

        status = new TextView(this);
        status.setTextSize(15);
        status.setGravity(Gravity.CENTER);
        root.addView(status);

        busy = new ProgressBar(this);
        busy.setIndeterminate(true);
        busy.setVisibility(View.GONE);
        root.addView(busy);

        btnStart = button("开始游戏", v -> startGame());
        btnZip = button("选择游戏 ZIP", v -> pick(REQ_ZIP));
        btnDir = button("选择游戏目录", v -> pick(REQ_DIR));
        root.addView(btnStart);
        root.addView(btnZip);
        root.addView(btnDir);
        setContentView(root);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (status == null || importing || started) return;
        if (hasExe(gameDir())) { startGame(); return; }   // 已有游戏数据：直接进游戏，跳过启动页
        refresh();
    }

    private void refresh() {
        boolean ok = hasExe(gameDir());
        btnStart.setEnabled(ok && !importing);
        btnZip.setEnabled(!importing);
        btnDir.setEnabled(!importing);
        busy.setVisibility(importing ? View.VISIBLE : View.GONE);
        if (!importing) status.setText(ok ? "已载入游戏数据" : "尚未载入游戏数据");
    }

    private void startGame() {
        started = true;
        startActivity(new Intent(this, FlyingSBActivity.class));
        finish();   // 游戏退出后回到桌面，不再停在启动页
    }

    private void pick(int req) {
        Intent i;
        if (req == REQ_ZIP) {
            i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            i.putExtra(Intent.EXTRA_MIME_TYPES, new String[] { "application/zip", "application/x-zip-compressed", "application/octet-stream" });
        } else {
            i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        }
        startActivityForResult(i, req);
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        importing = true;
        copiedBytes = 0;
        copiedFiles = 0;
        setStatus("正在导入……");
        refresh();
        new Thread(() -> {
            String err = null;
            File tmp = new File(baseDir(), "game_tmp");
            try {
                deleteAll(tmp);
                tmp.mkdirs();
                if (req == REQ_ZIP) extractZip(uri, tmp);
                else copyTree(uri, DocumentsContract.getTreeDocumentId(uri), tmp);
                File found = findExeDir(tmp, 0);
                if (found == null) throw new Exception("没有找到 FlyingSB.exe，请选择含有原版游戏的 ZIP 或目录。");
                File dst = gameDir();
                deleteAll(dst);
                if (!found.renameTo(dst)) throw new Exception("无法保存游戏数据。");
            } catch (Exception e) {
                err = e.getMessage() != null ? e.getMessage() : e.toString();
            } finally {
                deleteAll(tmp);
            }
            final String msg = err;
            runOnUiThread(() -> {
                importing = false;
                if (msg == null) { startGame(); return; }   // 导入完成直接进游戏
                refresh();
                status.setText("导入失败：" + msg);
            });
        }).start();
    }

    private void setStatus(String s) { status.setText(s); }

    private void progress(long add) {
        copiedBytes += add;
        long now = System.currentTimeMillis();
        if (now - lastUi > 200) {
            lastUi = now;
            final String s = "正在导入……已复制 " + copiedFiles + " 个文件，" + (copiedBytes >> 20) + " MB";
            runOnUiThread(() -> status.setText(s));
        }
    }

    private void copy(InputStream in, File out) throws Exception {
        out.getParentFile().mkdirs();
        try (OutputStream os = new FileOutputStream(out)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) { os.write(buf, 0, n); progress(n); }
        }
        copiedFiles++;
    }

    private void extractZip(Uri uri, File dest) throws Exception {
        String base = dest.getCanonicalPath() + File.separator;
        // 中文 Windows 打的包文件名多为 GBK；带 UTF-8 标记的条目会自动按 UTF-8 解
        try (InputStream raw = getContentResolver().openInputStream(uri);
             ZipInputStream zin = new ZipInputStream(raw, Charset.forName("GBK"))) {
            ZipEntry e;
            while ((e = zin.getNextEntry()) != null) {
                if (e.isDirectory()) continue;
                File out = new File(dest, e.getName());
                if (!out.getCanonicalPath().startsWith(base)) continue;   // 防止 ../ 越界
                copy(zin, out);
            }
        }
    }

    private void copyTree(Uri tree, String docId, File dest) throws Exception {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
        String[] cols = { DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE };
        try (Cursor c = getContentResolver().query(children, cols, null, null, null)) {
            if (c == null) return;
            while (c.moveToNext()) {
                String id = c.getString(0), name = c.getString(1), mime = c.getString(2);
                File out = new File(dest, name);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    out.mkdirs();
                    copyTree(tree, id, out);
                } else {
                    try (InputStream in = getContentResolver().openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, id))) {
                        copy(in, out);
                    }
                }
            }
        }
    }

    /** ZIP 里常带一层外壳目录：向下找最多三层，找到含 FlyingSB.exe 的那一层 */
    private static File findExeDir(File dir, int depth) {
        if (hasExe(dir)) return dir;
        if (depth >= 3) return null;
        File[] subs = dir.listFiles();
        if (subs == null) return null;
        for (File f : subs) {
            if (f.isDirectory()) {
                File r = findExeDir(f, depth + 1);
                if (r != null) return r;
            }
        }
        return null;
    }

    private static void deleteAll(File f) {
        if (!f.exists()) return;
        File[] subs = f.listFiles();
        if (subs != null) for (File s : subs) deleteAll(s);
        f.delete();
    }
}
