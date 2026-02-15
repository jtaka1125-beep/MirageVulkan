#!/usr/bin/env python3
"""MirageSystem Macro Editor - Entry Point"""
import os
import webview
from backend.api import MacroEditorAPI

def main():
    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    api = MacroEditorAPI()
    window = webview.create_window(
        'MirageSystem Macro Editor',
        url='frontend/index.html',
        js_api=api,
        width=1280, height=800,
        min_size=(960, 600)
    )
    webview.start(debug=True)

if __name__ == '__main__':
    main()
