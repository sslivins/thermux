#!/usr/bin/env python3
"""
HTML/CSS/JS Minifier for ESP32 Web Server
Removes whitespace, comments, and unnecessary characters to reduce size.
"""

import re
import sys
import os

def minify_html(html):
    """Minify HTML content."""
    # Remove HTML comments (but not conditional comments)
    html = re.sub(r'<!--(?!\[if).*?-->', '', html, flags=re.DOTALL)
    
    # Remove whitespace between tags
    html = re.sub(r'>\s+<', '><', html)
    
    # Remove leading/trailing whitespace on lines
    html = re.sub(r'^\s+', '', html, flags=re.MULTILINE)
    html = re.sub(r'\s+$', '', html, flags=re.MULTILINE)
    
    # Collapse multiple whitespace to single space
    html = re.sub(r'\s{2,}', ' ', html)
    
    # Remove newlines
    html = html.replace('\n', '').replace('\r', '')
    
    # Minify inline CSS
    html = minify_inline_css(html)
    
    # Minify inline JS
    html = minify_inline_js(html)
    
    return html.strip()

def minify_inline_css(html):
    """Minify CSS within <style> tags."""
    def minify_css_block(match):
        css = match.group(1)
        # Remove CSS comments
        css = re.sub(r'/\*.*?\*/', '', css, flags=re.DOTALL)
        # Remove whitespace around special characters
        css = re.sub(r'\s*([{};:,>+~])\s*', r'\1', css)
        # Remove trailing semicolons before closing braces
        css = re.sub(r';}', '}', css)
        # Collapse whitespace
        css = re.sub(r'\s+', ' ', css)
        return '<style>' + css.strip() + '</style>'
    
    return re.sub(r'<style[^>]*>(.*?)</style>', minify_css_block, html, flags=re.DOTALL | re.IGNORECASE)

def minify_inline_js(html):
    """Minify JavaScript within <script> tags."""
    def minify_js_block(match):
        js = match.group(1)
        # Remove single-line comments (but be careful with URLs)
        js = re.sub(r'(?<!:)//[^\n]*', '', js)
        # Remove multi-line comments
        js = re.sub(r'/\*.*?\*/', '', js, flags=re.DOTALL)
        # Collapse whitespace (but preserve strings)
        # This is a simplified approach - doesn't handle all edge cases
        js = re.sub(r'\s+', ' ', js)
        # Remove spaces around operators (simplified)
        js = re.sub(r'\s*([{};:,=<>+\-*/&|!?])\s*', r'\1', js)
        # Fix function declarations
        js = re.sub(r'function\s*\(', 'function(', js)
        js = re.sub(r'\)\s*{', '){', js)
        js = re.sub(r'}\s*else', '}else', js)
        js = re.sub(r'else\s*{', 'else{', js)
        js = re.sub(r'}\s*catch', '}catch', js)
        js = re.sub(r'try\s*{', 'try{', js)
        # Fix async/await
        js = re.sub(r'async\s+function', 'async function', js)
        js = re.sub(r'await\s+', 'await ', js)
        # Fix const/let/var
        js = re.sub(r'(const|let|var)\s+', r'\1 ', js)
        js = re.sub(r'return\s+', 'return ', js)
        return '<script>' + js.strip() + '</script>'
    
    return re.sub(r'<script[^>]*>(.*?)</script>', minify_js_block, html, flags=re.DOTALL | re.IGNORECASE)

def main():
    if len(sys.argv) < 3:
        print("Usage: minify_html.py <input_dir> <output_dir>")
        sys.exit(1)
    
    input_dir = sys.argv[1]
    output_dir = sys.argv[2]
    
    # Create output directory if it doesn't exist
    os.makedirs(output_dir, exist_ok=True)
    
    # Process all HTML files
    for filename in os.listdir(input_dir):
        if filename.endswith('.html'):
            input_path = os.path.join(input_dir, filename)
            output_path = os.path.join(output_dir, filename)
            
            with open(input_path, 'r', encoding='utf-8') as f:
                content = f.read()
            
            original_size = len(content)
            minified = minify_html(content)
            minified_size = len(minified)
            
            with open(output_path, 'w', encoding='utf-8') as f:
                f.write(minified)
            
            reduction = (1 - minified_size / original_size) * 100
            print(f"  {filename}: {original_size} -> {minified_size} bytes ({reduction:.1f}% reduction)")

if __name__ == '__main__':
    main()
