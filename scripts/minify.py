Import("env")
print(">> Running precompress")

import os
import gzip
import shutil
import subprocess

source_dir = "www"
target_dir = "data"

def run_node_minification():
    print(">> Running Node.js minifier...")
    subprocess.run(
        ["node", "bundle/minify.js", source_dir, target_dir],
        check=True
    )

def compress_file(src_path, dst_path):
    with open(src_path, 'rb') as f_in:
        with gzip.open(dst_path, 'wb', compresslevel=9) as f_out:
            shutil.copyfileobj(f_in, f_out)

def clean_target_dir():
    for root, dirs, files in os.walk(target_dir):
        for file in files:
            if file.endswith(".gz") or file.endswith(".js") or file.endswith(".css") or file.endswith(".html"):
                os.remove(os.path.join(root, file))

def ensure_target_dir_structure():
    for root, dirs, files in os.walk(source_dir):
        rel_root = os.path.relpath(root, source_dir)
        target_root = os.path.join(target_dir, rel_root)
        os.makedirs(target_root, exist_ok=True)

def compress_minified_files():
    for root, dirs, files in os.walk(target_dir):
        for file in files:
            src_path = os.path.join(root, file)
            dst_path = src_path + ".gz"
            compress_file(src_path, dst_path)
            print(f"Compressed {src_path} -> {dst_path}")
            os.remove(src_path)


clean_target_dir()
ensure_target_dir_structure()
run_node_minification()
compress_minified_files()
print(">> precompress done")