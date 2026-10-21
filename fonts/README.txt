放在这里的字体文件会被程序优先使用（原生版和 Web 服务器都会找这个目录）。

仓库里不带字体文件。推荐自行放入宋体 simsun.ttc（从 Windows 的 C:\Windows\Fonts\ 复制）。
原版游戏请求的字体是「新細明體」(PMingLiU)，宋体是风格最接近的替代：
两者在 12、16 像素下都带手工点阵字形，FreeType 会直接取用这些点阵。

没有 simsun.ttc 时的行为：
- 桌面版和 Web 服务器：自动改用系统里的中文字体（Windows 上依次是微软正黑体、微软雅黑、黑体、楷体等），能显示中文，但没有点阵字形。
- Android：打包时 fonts/ 里的 simsun.ttc 会进 APK；没有的话只用系统自带的 Noto CJK 作后备，部分机型可能缺字。要用宋体请在打包前把它放进来。

注意：simsun.ttc 是微软的商业字体，只供在你自己的机器上本地使用，
不要把它提交进仓库或随项目分发（.gitignore 已排除 fonts/*.tt?）。
想换字体：把你合法拥有的字体文件放进来，并保持文件名 simsun.ttc，
或用环境变量 FLYINGSB_FONT / 存档目录的 font.txt / 服务器的 --font 指定路径。
