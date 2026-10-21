//! FlyingSB Web 服务器。
//!
//! - `/`、`/flyingsb.*`：托管 WASM 客户端（CMake 的 Emscripten 构建产物目录）
//! - `/api/manifest`：游戏目录文件清单，每行 `大小\t相对路径`（路径用 `/` 分隔）
//! - `/game/<相对路径>`：游戏文件，支持单段 Range 请求（浏览器端按需读取，不整体下载）
//!
//! 游戏数据不在客户端里：服务器只读取用户自己的游戏目录，服务器本身也不含任何原版资源。

use axum::{
    body::Body,
    extract::{Path as AxPath, State},
    http::{header, HeaderMap, HeaderValue, StatusCode, Uri},
    response::{IntoResponse, Redirect, Response},
    routing::get,
    Router,
};
use std::{
    collections::HashMap,
    path::{Path, PathBuf},
    process::exit,
    sync::Arc,
};
use tokio::io::{AsyncReadExt, AsyncSeekExt};
use tokio_util::io::ReaderStream;
use tower_http::{services::ServeDir, set_header::SetResponseHeaderLayer};

struct Entry {
    abs: PathBuf,
    size: u64,
}

struct App {
    files: HashMap<String, Entry>,
    manifest: String,
}

/// 服务器可以为浏览器提供的中文字体（客户端没有系统字体）；按顺序找第一个存在的
const FONT_CANDIDATES: &[&str] = &[
    "C:/Windows/Fonts/simsun.ttc",
    "C:/Windows/Fonts/msjh.ttc",
    "C:/Windows/Fonts/msyh.ttc",
    "C:/Windows/Fonts/simhei.ttf",
    "/System/Library/Fonts/PingFang.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
];

/// 项目 fonts/ 目录的可能位置：当前目录、上级目录，以及可执行文件旁/上几级
fn local_font_dirs() -> Vec<PathBuf> {
    let mut v = vec![PathBuf::from("fonts"), PathBuf::from("../fonts")];
    if let Ok(exe) = std::env::current_exe() {
        let mut d = exe.parent().map(Path::to_path_buf);
        for _ in 0..4 {
            if let Some(dir) = d {
                v.push(dir.join("fonts"));
                d = dir.parent().map(Path::to_path_buf);
            } else {
                break;
            }
        }
    }
    v
}

/// 从证书的 SAN 里取第一个可用于跳转的主机：IPv6 地址（带方括号）或域名；IPv4 地址返回 None
fn cert_host(path: &Path) -> Option<String> {
    use x509_parser::extensions::GeneralName;
    let data = std::fs::read(path).ok()?;
    let (_, pem) = x509_parser::pem::parse_x509_pem(&data).ok()?;
    let cert = pem.parse_x509().ok()?;
    let san = cert.subject_alternative_name().ok()??;
    for gn in &san.value.general_names {
        match gn {
            GeneralName::IPAddress(b) if b.len() == 16 => {
                let mut a = [0u8; 16];
                a.copy_from_slice(b);
                return Some(format!("[{}]", std::net::Ipv6Addr::from(a)));
            }
            GeneralName::DNSName(d) => return Some((*d).to_string()),
            _ => {}
        }
    }
    None
}

fn scan(dir: &Path, rel: &str, out: &mut Vec<(String, PathBuf, u64)>) {
    let Ok(rd) = std::fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        let r = if rel.is_empty() { name.clone() } else { format!("{rel}/{name}") };
        let Ok(md) = e.metadata() else { continue };
        if md.is_dir() {
            scan(&e.path(), &r, out);
        } else if md.is_file() {
            out.push((r, e.path(), md.len()));
        }
    }
}

fn usage() -> ! {
    eprintln!(
        "用法: flyingsb-server --game <原版游戏目录> [--web <客户端目录>] [--port 8080] [--bind 0.0.0.0] [--font <字体文件>] [--tls-cert <证书.pem> --tls-key <私钥.pem>]\n\
         \n  --game  含 FlyingSB.exe 与全部资源的原版游戏目录（必填）\n  \
         --web   WASM 客户端目录（默认 ../build/web，即 CMake Emscripten 构建输出）\n  \
         --font  提供给浏览器的中文字体（默认：项目 fonts/simsun.ttc，其次系统字体）\n  \
         --bind  监听地址，默认 0.0.0.0 表示同时监听 IPv4 和 IPv6 的所有地址\n  \
         --tls-cert/--tls-key  启用 HTTPS（PEM 文件）；此时默认端口为 443\n  \
         启用 HTTPS 且证书是 IPv6 地址/域名时：IPv6 的 --port 提供 HTTPS，IPv4 的同一端口提供 HTTP 并跳转到证书里的地址"
    );
    exit(2)
}

#[tokio::main]
async fn main() {
    let mut game: Option<PathBuf> = None;
    let mut web = PathBuf::from("../build/web");
    let mut port: u16 = 8080;
    let mut bind = "0.0.0.0".to_string();
    let mut font: Option<PathBuf> = None;
    let mut tls_cert: Option<PathBuf> = None;
    let mut tls_key: Option<PathBuf> = None;
    let mut port_set = false;
    let mut it = std::env::args().skip(1);
    while let Some(a) = it.next() {
        match a.as_str() {
            "--game" => game = it.next().map(PathBuf::from),
            "--web" => web = it.next().map(PathBuf::from).unwrap_or_else(|| usage()),
            "--port" => {
                port = it.next().and_then(|v| v.parse().ok()).unwrap_or_else(|| usage());
                port_set = true;
            }
            "--tls-cert" => tls_cert = it.next().map(PathBuf::from),
            "--tls-key" => tls_key = it.next().map(PathBuf::from),
            "--bind" => bind = it.next().unwrap_or_else(|| usage()),
            "--font" => font = it.next().map(PathBuf::from),
            _ => usage(),
        }
    }
    let game = game.unwrap_or_else(|| usage());
    if tls_cert.is_some() != tls_key.is_some() {
        eprintln!("错误: --tls-cert 和 --tls-key 必须同时给出。");
        exit(2);
    }
    if tls_cert.is_some() && !port_set {
        port = 443; // HTTPS 默认端口
    }
    // 跳转目标取自证书里的地址：IPv6（加方括号）或域名；证书是 IPv4 地址时不做跳转
    let redirect_host: Option<String> = tls_cert.as_deref().and_then(cert_host);

    let mut list = Vec::new();
    scan(&game, "", &mut list);
    if !list.iter().any(|(r, _, _)| r.eq_ignore_ascii_case("FlyingSB.exe")) {
        eprintln!("错误: 在 {} 中找不到 FlyingSB.exe，请用 --game 指向你自己拥有的原版游戏目录。", game.display());
        exit(1);
    }
    // 字体：--font 指定 > 项目 fonts/simsun.ttc > 系统字体；主字体之后再附最多两个后备（主字体缺字时用）
    let mut fonts: Vec<PathBuf> = Vec::new();
    let mut push_font = |p: PathBuf| {
        // 用真实路径去重：同一个文件可能从 fonts/、../fonts、系统目录等多处出现
        let Ok(real) = p.canonicalize() else { return };
        let same = |a: &PathBuf, b: &PathBuf| match (std::fs::metadata(a), std::fs::metadata(b)) {
            (Ok(x), Ok(y)) => x.len() == y.len() && a.file_name() == b.file_name(),
            _ => false,
        };
        if real.is_file() && !fonts.iter().any(|f| *f == real || same(f, &real)) && fonts.len() < 3 {
            fonts.push(real);
        }
    };
    if let Some(f) = font {
        push_font(f);
    }
    for dir in local_font_dirs() {
        push_font(dir.join("simsun.ttc"));
    }
    for c in FONT_CANDIDATES {
        push_font(PathBuf::from(c));
    }
    if fonts.is_empty() {
        eprintln!("警告: 没找到中文字体，浏览器里的游戏文字将无法显示。请把 simsun.ttc 放进 fonts/ 目录，或用 --font 指定。");
    }
    for (i, f) in fonts.iter().enumerate() {
        if let Ok(md) = std::fs::metadata(f) {
            let name = if i == 0 { "@font".to_string() } else { format!("@font{}", i + 1) };
            list.push((name, f.clone(), md.len()));
            eprintln!("中文字体[{}]: {}", i + 1, f.display());
        }
    }

    let mut manifest = String::new();
    let mut files = HashMap::new();
    for (rel, abs, size) in list {
        manifest.push_str(&format!("{size}\t{rel}\n"));
        files.insert(rel, Entry { abs, size });
    }
    eprintln!("游戏目录: {}（{} 个文件）", game.display(), files.len());
    let app = Arc::new(App { files, manifest });

    let router = Router::new()
        .route(
            "/",
            get(|uri: Uri| async move {
                // 保留查询参数（?skip-video 等）
                let q = uri.query().map(|q| format!("?{q}")).unwrap_or_default();
                Redirect::temporary(&format!("/flyingsb.html{q}"))
            }),
        )
        .route("/api/manifest", get(manifest_handler))
        .route("/game/{*path}", get(game_handler))
        .with_state(app)
        .fallback_service(ServeDir::new(&web))
        // SharedArrayBuffer / pthread 需要跨源隔离
        .layer(SetResponseHeaderLayer::overriding(
            header::HeaderName::from_static("cross-origin-opener-policy"),
            HeaderValue::from_static("same-origin"),
        ))
        .layer(SetResponseHeaderLayer::overriding(
            header::HeaderName::from_static("cross-origin-embedder-policy"),
            HeaderValue::from_static("require-corp"),
        ))
        .layer(SetResponseHeaderLayer::overriding(
            header::HeaderName::from_static("cross-origin-resource-policy"),
            HeaderValue::from_static("same-origin"),
        ));

    // 默认同时监听 IPv4 和 IPv6：Windows 上 [::] 不是双栈，所以要开两个监听
    let addrs: Vec<String> = if bind == "0.0.0.0" {
        if redirect_host.is_some() {
            // IPv4 的这个端口留给 HTTP 跳转，HTTPS 只在 IPv6 上监听
            vec![format!("[::]:{port}")]
        } else {
            vec![format!("0.0.0.0:{port}"), format!("[::]:{port}")]
        }
    } else if bind.contains(':') && !bind.starts_with('[') {
        vec![format!("[{bind}]:{port}")]
    } else {
        vec![format!("{bind}:{port}")]
    };
    let mut listeners = Vec::new();
    for a in &addrs {
        match std::net::TcpListener::bind(a) {
            Ok(l) => {
                l.set_nonblocking(true).unwrap();
                eprintln!("监听 {a}");
                listeners.push(l);
            }
            Err(e) => eprintln!("警告: 无法监听 {a}: {e}"),
        }
    }
    if listeners.is_empty() {
        eprintln!("错误: 没有任何地址可监听。");
        exit(1);
    }

    let tls = match (&tls_cert, &tls_key) {
        (Some(c), Some(k)) => {
            rustls::crypto::ring::default_provider().install_default().ok();
            match axum_server::tls_rustls::RustlsConfig::from_pem_file(c, k).await {
                Ok(cfg) => Some(cfg),
                Err(e) => {
                    eprintln!("错误: 读取证书/私钥失败: {e}");
                    exit(1);
                }
            }
        }
        _ => None,
    };

    eprintln!("客户端目录: {}", web.display());
    if tls.is_some() {
        eprintln!("HTTPS 已启用。浏览器须用证书里的地址访问，例如 https://[<IPv6>]:{port}/");
    } else {
        eprintln!("本机请打开 http://localhost:{port}/");
        eprintln!(
            "注意：浏览器只在安全上下文（https 或 localhost）里提供线程所需的 SharedArrayBuffer；\n\
             用局域网 IP / 域名经 http 访问时，页面不是跨源隔离的，游戏无法运行，请用 --tls-cert/--tls-key 启用 HTTPS。"
        );
    }

    let mut tasks = Vec::new();

    // 仅 IPv4 的明文 HTTP 监听：把所有请求（保留路径和查询参数）跳转到 HTTPS 地址
    if let Some(host) = redirect_host {
        let target = format!("https://{host}:{port}");
        let addr = format!("0.0.0.0:{port}");
        match std::net::TcpListener::bind(&addr) {
            Ok(l) => {
                l.set_nonblocking(true).unwrap();
                eprintln!("监听 {addr}（HTTP，跳转到 {target}）");
                let app = Router::new().fallback(move |uri: Uri| {
                    let target = target.clone();
                    async move {
                        let pq = uri.path_and_query().map(|p| p.as_str()).unwrap_or("/");
                        Redirect::temporary(&format!("{target}{pq}"))
                    }
                });
                tasks.push(tokio::spawn(async move {
                    let l = tokio::net::TcpListener::from_std(l).unwrap();
                    axum::serve(l, app).await
                }));
            }
            Err(e) => eprintln!("警告: 无法监听 {addr}: {e}"),
        }
    }

    for l in listeners {
        let router = router.clone();
        match tls.clone() {
            Some(cfg) => tasks.push(tokio::spawn(async move {
                axum_server::from_tcp_rustls(l, cfg)?.serve(router.into_make_service()).await
            })),
            None => tasks.push(tokio::spawn(async move {
                let l = tokio::net::TcpListener::from_std(l).unwrap();
                axum::serve(l, router).await
            })),
        }
    }
    for t in tasks {
        let _ = t.await;
    }
}

async fn manifest_handler(State(app): State<Arc<App>>) -> Response {
    (
        [(header::CONTENT_TYPE, "text/plain; charset=utf-8"), (header::CACHE_CONTROL, "no-cache")],
        app.manifest.clone(),
    )
        .into_response()
}

/// 解析单段 `Range: bytes=a-b`，返回闭区间 [start, end]
fn parse_range(h: &str, size: u64) -> Option<(u64, u64)> {
    let spec = h.strip_prefix("bytes=")?;
    if spec.contains(',') || size == 0 {
        return None;
    }
    let (a, b) = spec.split_once('-')?;
    let (start, end) = if a.is_empty() {
        let n: u64 = b.parse().ok()?;
        (size.saturating_sub(n), size - 1)
    } else {
        let s: u64 = a.parse().ok()?;
        let e: u64 = if b.is_empty() { size - 1 } else { b.parse().ok()? };
        (s, e.min(size - 1))
    };
    (start <= end && start < size).then_some((start, end))
}

async fn game_handler(State(app): State<Arc<App>>, AxPath(path): AxPath<String>, headers: HeaderMap) -> Response {
    // 只允许清单里存在的文件，天然避免目录穿越
    let Some(entry) = app.files.get(&path) else {
        return StatusCode::NOT_FOUND.into_response();
    };
    let mut file = match tokio::fs::File::open(&entry.abs).await {
        Ok(f) => f,
        Err(_) => return StatusCode::NOT_FOUND.into_response(),
    };
    let range = headers.get(header::RANGE).and_then(|v| v.to_str().ok()).map(|v| parse_range(v, entry.size));
    let base = |status: StatusCode| {
        Response::builder()
            .status(status)
            .header(header::CONTENT_TYPE, "application/octet-stream")
            .header(header::ACCEPT_RANGES, "bytes")
            .header(header::CACHE_CONTROL, "no-cache")
    };
    match range {
        Some(None) => base(StatusCode::RANGE_NOT_SATISFIABLE)
            .header(header::CONTENT_RANGE, format!("bytes */{}", entry.size))
            .body(Body::empty())
            .unwrap(),
        Some(Some((s, e))) => {
            if file.seek(std::io::SeekFrom::Start(s)).await.is_err() {
                return StatusCode::INTERNAL_SERVER_ERROR.into_response();
            }
            let len = e - s + 1;
            base(StatusCode::PARTIAL_CONTENT)
                .header(header::CONTENT_RANGE, format!("bytes {s}-{e}/{}", entry.size))
                .header(header::CONTENT_LENGTH, len)
                .body(Body::from_stream(ReaderStream::new(file.take(len))))
                .unwrap()
        }
        None => base(StatusCode::OK)
            .header(header::CONTENT_LENGTH, entry.size)
            .body(Body::from_stream(ReaderStream::new(file)))
            .unwrap(),
    }
}
