import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { scaffoldProject } from "../bin/create-lamp-app.ts";

test("scaffoldProject creates TypeScript + Node Lamp boilerplate", () => {
  const tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), "create-lamp-app-test-"));
  const targetDir = path.join(tmpRoot, "demo-service");

  try {
    scaffoldProject({
      projectName: "demo-service",
      targetDir,
      install: false,
    });

    const expectedFiles = [
      "package.json",
      "tsconfig.json",
      ".gitignore",
      "README.md",
      path.join("src", "app.ts"),
      path.join("src", "router.ts"),
      path.join("src", "registry.ts"),
    ];

    for (const rel of expectedFiles) {
      assert.equal(fs.existsSync(path.join(targetDir, rel)), true, `Missing ${rel}`);
    }
    assert.equal(fs.existsSync(path.join(targetDir, "scripts", "client.ts")), false);

    const pkg = JSON.parse(fs.readFileSync(path.join(targetDir, "package.json"), "utf8"));
    assert.equal(pkg.name, "demo-service");
    assert.equal(pkg.type, "module");
    assert.equal(pkg.dependencies["lamp-app"], "^0.4.1");
    assert.equal("lamp-client" in pkg.dependencies, false);
    assert.equal(pkg.scripts.dev, "lamp serve src/app.ts --port 3000 --watch");
    assert.equal(pkg.scripts.build, "lamp build src/app.ts --out dist/app.js");
    assert.equal(pkg.scripts.start, "lamp serve dist/app.js --port 3000");

    const appSource = fs.readFileSync(path.join(targetDir, "src", "app.ts"), "utf8");
    assert.match(appSource, /from "lamp-app"/);
    assert.match(appSource, /createLampApp/);
    assert.match(appSource, /registerRouter\(rootRouter\)/);
  } finally {
    fs.rmSync(tmpRoot, { recursive: true, force: true });
  }
});

test("scaffoldProject refuses to overwrite non-empty directory", () => {
  const tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), "create-lamp-app-nonempty-"));
  fs.writeFileSync(path.join(tmpRoot, "existing.txt"), "keep", "utf8");

  try {
    assert.throws(
      () => scaffoldProject({ projectName: "existing", targetDir: tmpRoot, install: false }),
      /not empty/
    );
  } finally {
    fs.rmSync(tmpRoot, { recursive: true, force: true });
  }
});
