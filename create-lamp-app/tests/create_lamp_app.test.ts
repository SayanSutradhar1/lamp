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
      path.join("scripts", "client.ts"),
    ];

    for (const rel of expectedFiles) {
      assert.equal(fs.existsSync(path.join(targetDir, rel)), true, `Missing ${rel}`);
    }

    const pkg = JSON.parse(fs.readFileSync(path.join(targetDir, "package.json"), "utf8"));
    assert.equal(pkg.name, "demo-service");
    assert.equal(pkg.type, "module");
    assert.equal(pkg.dependencies["lamp-app"], "^0.3.1");
    assert.equal(pkg.dependencies["lamp-client"], "^0.2.1");
    assert.equal(pkg.scripts.dev, "lamp serve src/app.ts --port 3000");

    const appSource = fs.readFileSync(path.join(targetDir, "src", "app.ts"), "utf8");
    assert.match(appSource, /from "lamp-app"/);
    assert.match(appSource, /createLampApp/);
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
