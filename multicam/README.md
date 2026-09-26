# multicam 插件

米家双路摄像头的插件源码、Web 设置面板与打包配置。安装、使用与功能说明见[仓库根 README](../README.md)。

## 打包

```bash
docker run --rm -v "$(pwd)":/work ainice-build make -C multicam package
```

产物为 `multicam-<版本>.plugin`（tar.gz 结构：`plugin.json` + `multicam.app` 入口 + `bin/` go2rtc + `libs/` FFmpeg 运行库 + `etc/` 默认配置 + `www/` 面板）。

## 源码结构

```
src/main.c     启动、拉流循环、置灰与自愈
src/config.c   配置加载与首次启动引导
src/g2r.c      go2rtc 实例生命周期、凭证与 token 自持
src/capture.c  RTSP 拉流与 HEVC/H264/MJPEG 软解（GOP 窗口解码）
src/compose.c  画布合成（左右并排裁切 / 网格）
src/push.c     SDK bitstream 推流
src/auth.c     米家账号授权编排
src/panel.c    面板 bridge（状态/配置/取景器快照/换源/授权）
src/vendor/    cJSON 与米家授权客户端（mhcamera，MIT）
www/           设置面板（原生 HTML/CSS/JS，无构建）
```

版本号在 `Makefile` 与 `plugin.json` 中维护，更新日志见 `release/release.json`。
