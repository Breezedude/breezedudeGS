const fs = require("fs");
const fsp = fs.promises; // use fs.promises for cleaner async
const path = require("path");
const terser = require("terser");
const { minify: minifyHTML } = require("html-minifier-terser");
const CleanCSS = require("clean-css");

const inputDir = process.argv[2];
const outputDir = process.argv[3];

function ensureDirExists(dirPath) {
  fs.mkdirSync(dirPath, { recursive: true });
}

async function copyFile(srcPath, relPath) {
  const outPath = path.join(outputDir, relPath);
  await fsp.mkdir(path.dirname(outPath), { recursive: true });
  await fsp.copyFile(srcPath, outPath);
  console.log(`Copied: ${relPath}`);
}

async function minifyFile(filePath, relPath) {
  const ext = path.extname(filePath);
  const outPath = path.join(outputDir, relPath);
  ensureDirExists(path.dirname(outPath));

  const code = fs.readFileSync(filePath, "utf-8");

  if (ext === ".js") {
    const result = await terser.minify(code, { mangle: true, compress: true });
    fs.writeFileSync(outPath, result.code);
  } else if (ext === ".css") {
    const result = new CleanCSS({ level: 2 }).minify(code);
    fs.writeFileSync(outPath, result.styles);
  } else if (ext === ".html") {
    const result = await minifyHTML(code, {
      collapseWhitespace: true,
      removeComments: true,
      minifyJS: true,
      minifyCSS: true,
    });
    fs.writeFileSync(outPath, result);
  }
}

async function walkAndMinify(dir, relRoot = "") {
  const entries = fs.readdirSync(dir, { withFileTypes: true });
  for (const entry of entries) {
    const fullPath = path.join(dir, entry.name);
    const relPath = path.join(relRoot, entry.name);
    if (entry.isDirectory()) {
      await walkAndMinify(fullPath, relPath);
    } else if ([".js", ".css", ".html"].includes(path.extname(entry.name))) {
      await minifyFile(fullPath, relPath);
    } else {
      await copyFile(fullPath, relPath);
    }
  }
}

(async () => {
  await walkAndMinify(inputDir);
})();