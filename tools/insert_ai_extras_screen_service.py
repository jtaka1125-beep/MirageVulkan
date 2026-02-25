import pathlib,re
p=pathlib.Path(r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\capture\ScreenCaptureService.kt')
s=p.read_text(encoding='utf-8',errors='replace')
if 'EXTRA_AI_PORT' not in s:
    ins = (
        '        private const val EXTRA_AI_PORT = "ai_port"\n'
        '        private const val EXTRA_AI_WIDTH = "ai_width"\n'
        '        private const val EXTRA_AI_HEIGHT = "ai_height"\n'
        '        private const val EXTRA_AI_FPS = "ai_fps"\n'
        '        private const val EXTRA_AI_QUALITY = "ai_quality"\n'
    )
    s = re.sub(r'(\s*private const val EXTRA_ROUTE_MODE = "route_mode"\s*\r?\n)', r"\1"+ins, s, count=1)
    if 'EXTRA_AI_PORT' not in s:
        raise SystemExit('insert failed')
p.write_text(s,encoding='utf-8')
print('inserted')
