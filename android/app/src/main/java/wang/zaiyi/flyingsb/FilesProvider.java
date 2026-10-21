package wang.zaiyi.flyingsb;

import android.content.res.AssetFileDescriptor;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.graphics.Point;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.provider.DocumentsProvider;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;

/**
 * 把应用自己的文件目录（游戏数据、存档、配置、日志）作为一个存储位置交给系统的"文件"应用：
 * 不需要任何存储权限，在文件应用的侧栏里就能浏览、复制、替换、删除这些文件。
 * 文档 ID 就是文件的绝对路径。
 */
public class FilesProvider extends DocumentsProvider {
    private static final String[] ROOT_COLS = { Root.COLUMN_ROOT_ID, Root.COLUMN_FLAGS, Root.COLUMN_ICON, Root.COLUMN_TITLE,
                                                Root.COLUMN_SUMMARY, Root.COLUMN_DOCUMENT_ID, Root.COLUMN_MIME_TYPES };
    private static final String[] DOC_COLS = { Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE,
                                               Document.COLUMN_FLAGS, Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED };

    private File base;

    @Override
    public boolean onCreate() {
        File d = getContext().getExternalFilesDir(null);
        base = d != null ? d : getContext().getFilesDir();
        return true;
    }

    private File fileOf(String id) throws FileNotFoundException {
        File f = new File(id);
        try {
            String root = base.getCanonicalPath();
            String p = f.getCanonicalPath();
            if (!p.equals(root) && !p.startsWith(root + File.separator)) throw new FileNotFoundException("越界: " + id);
        } catch (IOException e) {
            throw new FileNotFoundException(id);
        }
        return f;
    }

    private static String mimeOf(File f) {
        if (f.isDirectory()) return Document.MIME_TYPE_DIR;
        String n = f.getName();
        int i = n.lastIndexOf('.');
        if (i >= 0) {
            String m = MimeTypeMap.getSingleton().getMimeTypeFromExtension(n.substring(i + 1).toLowerCase());
            if (m != null) return m;
        }
        return "application/octet-stream";
    }

    private void addRow(MatrixCursor c, File f) {
        int flags = Document.FLAG_SUPPORTS_DELETE | Document.FLAG_SUPPORTS_RENAME;
        if (f.isDirectory()) flags |= Document.FLAG_DIR_SUPPORTS_CREATE;
        else flags |= Document.FLAG_SUPPORTS_WRITE;
        MatrixCursor.RowBuilder r = c.newRow();
        r.add(Document.COLUMN_DOCUMENT_ID, f.getAbsolutePath());
        r.add(Document.COLUMN_DISPLAY_NAME, f.equals(base) ? "FlyingSB" : f.getName());
        r.add(Document.COLUMN_MIME_TYPE, mimeOf(f));
        r.add(Document.COLUMN_FLAGS, flags);
        r.add(Document.COLUMN_SIZE, f.length());
        r.add(Document.COLUMN_LAST_MODIFIED, f.lastModified());
    }

    @Override
    public Cursor queryRoots(String[] projection) {
        MatrixCursor c = new MatrixCursor(projection != null ? projection : ROOT_COLS);
        MatrixCursor.RowBuilder r = c.newRow();
        r.add(Root.COLUMN_ROOT_ID, "flyingsb");
        r.add(Root.COLUMN_FLAGS, Root.FLAG_SUPPORTS_CREATE | Root.FLAG_SUPPORTS_IS_CHILD);
        r.add(Root.COLUMN_ICON, getContext().getApplicationInfo().icon);
        r.add(Root.COLUMN_TITLE, "幻想西游记");
        r.add(Root.COLUMN_SUMMARY, "游戏数据、存档、配置");
        r.add(Root.COLUMN_DOCUMENT_ID, base.getAbsolutePath());
        r.add(Root.COLUMN_MIME_TYPES, "*/*");
        return c;
    }

    @Override
    public Cursor queryDocument(String id, String[] projection) throws FileNotFoundException {
        MatrixCursor c = new MatrixCursor(projection != null ? projection : DOC_COLS);
        addRow(c, fileOf(id));
        return c;
    }

    @Override
    public Cursor queryChildDocuments(String parentId, String[] projection, String sortOrder) throws FileNotFoundException {
        MatrixCursor c = new MatrixCursor(projection != null ? projection : DOC_COLS);
        File[] kids = fileOf(parentId).listFiles();
        if (kids != null) for (File k : kids) addRow(c, k);
        return c;
    }

    @Override
    public ParcelFileDescriptor openDocument(String id, String mode, CancellationSignal signal) throws FileNotFoundException {
        return ParcelFileDescriptor.open(fileOf(id), ParcelFileDescriptor.parseMode(mode));
    }

    @Override
    public String createDocument(String parentId, String mimeType, String displayName) throws FileNotFoundException {
        File dir = fileOf(parentId);
        File f = new File(dir, displayName);
        try {
            if (Document.MIME_TYPE_DIR.equals(mimeType)) { if (!f.mkdir()) throw new IOException(); }
            else if (!f.createNewFile()) throw new IOException();
        } catch (IOException e) {
            throw new FileNotFoundException("无法创建 " + displayName);
        }
        return f.getAbsolutePath();
    }

    private static boolean deleteAll(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteAll(k);
        return f.delete();
    }

    @Override
    public void deleteDocument(String id) throws FileNotFoundException {
        File f = fileOf(id);
        if (f.equals(base) || !deleteAll(f)) throw new FileNotFoundException("无法删除 " + id);
    }

    @Override
    public String renameDocument(String id, String displayName) throws FileNotFoundException {
        File f = fileOf(id);
        File t = new File(f.getParentFile(), displayName);
        if (f.equals(base) || !f.renameTo(t)) throw new FileNotFoundException("无法重命名 " + id);
        return t.getAbsolutePath();
    }

    @Override
    public boolean isChildDocument(String parentId, String documentId) {
        return documentId.equals(parentId) || documentId.startsWith(parentId + File.separator);
    }

    @Override
    public String getDocumentType(String id) throws FileNotFoundException {
        return mimeOf(fileOf(id));
    }
}
