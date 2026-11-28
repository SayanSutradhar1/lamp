#!/usr/bin/env node
// ============================================================
// create-lamp-app — Project Scaffolder for the Lamp Framework
// Usage:
//   npx create-lamp-app@latest [project-name] [--yes|-y] [--no-install]
// ============================================================

import { spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { createInterface } from "node:readline/promises";
import { fileURLToPath } from "node:url";

export interface ScaffoldOptions {
  readonly projectName: string;
  readonly targetDir: string;
  readonly install: boolean;
}

function toValidPackageName(name: string): string {
  return (
    name
      .trim()
      .toLowerCase()
      .replace(/\s+/g, "-")
      .replace(/^[._]+/, "")
      .replace(/[^a-z0-9-~._]+/g, "-")
      .replace(/^-+|-+$/g, "") || "my-lamp-app"
  );
}

function printHelp(): void {
  console.log(
    "Usage: create-lamp-app [project-name] [options]\n\n" +
      "Options:\n" +
      "  -y, --yes         Accept defaults without prompting\n" +
      "  --no-install      Skip running npm install\n" +
      "  -h, --help        Show this help message"
  );
}

/**
 * Writes the Lamp TypeScript + Node.js project boilerplate into `targetDir`.
 */
export function scaffoldProject(options: ScaffoldOptions): void {
  const { projectName, targetDir } = options;
  const pkgName = toValidPackageName(path.basename(projectName));

  if (fs.existsSync(targetDir)) {
    const existing = fs.readdirSync(targetDir);
    if (existing.length > 0) {
      throw new Error(`Target directory "${targetDir}" already exists and is not empty.`);
    }
  }

  fs.mkdirSync(path.join(targetDir, "src"), { recursive: true });

  const packageJson = {
    name: pkgName,
    version: "0.1.0",
    private: true,
    type: "module",
    scripts: {
      dev: "lamp serve src/app.ts --port 3000 --watch",
      build: "lamp build src/app.ts --out dist/app.js",
      start: "lamp serve dist/app.js --port 3000",
      typecheck: "tsc --noEmit",
    },
    dependencies: {
      "lamp-app": "^0.4.2",
    },
    devDependencies: {
      "@types/node": "^24.0.0",
      typescript: "^5.8.3",
    },
    engines: {
      node: ">=22.18.0",
    },
  };

  const tsConfig = {
    compilerOptions: {
      target: "ES2022",
      module: "NodeNext",
      moduleResolution: "NodeNext",
      strict: true,
      erasableSyntaxOnly: true,
      allowImportingTsExtensions: true,
      noEmit: true,
      skipLibCheck: true,
      forceConsistentCasingInFileNames: true,
      verbatimModuleSyntax: true,
    },
    include: ["src/**/*.ts"],
  };

  const gitignore = [
    "node_modules/",
    "dist/",
    ".env",
    ".env.*",
    "*.log",
    ".DS_Store",
    "",
  ].join("\n");

  const registryTs = `import { createErrorHandler, createLampRegistry, s } from "lamp-app";

const rootErrorHandler = createErrorHandler({
  log: true,
});

export const rootRegistry = createLampRegistry({ onError: rootErrorHandler })
  .add("ping", {
    description: "Health check operation",
    input: s.optional(s.object({})),
    output: s.object({ ok: s.boolean(), runtime: s.string() }),
    handler: async () => ({
      ok: true,
      runtime: "lamp-v8",
    }),
  });
`;

  const routerTs = `import { createErrorHandler, createLampRouter } from "lamp-app";
import { rootRegistry } from "./registry.ts";

const appErrorHandler = createErrorHandler({
  log: true,
});

export const rootRouter = createLampRouter({
  description: "Root application router",
  target: "app",
  registry: [rootRegistry],
  nesting: [],
  options: {
    onError: appErrorHandler,
  },
});
`;

  const appTs = `import { createLampApp } from "lamp-app";
import { rootRouter } from "./router.ts";

const app = createLampApp({
  transport: "lmp1-binary",
  serialization: "lcb1",
  execution: "parallel-dynamic",
  log: true,
  cache: {
    enabled: true,
    defaultTtl: 60,
    maxTtlLimitSeconds: 300,
  },
});

app.registerRouter(rootRouter);

await app.start();
`;

  const readme = `# ${pkgName}

Backend project built with **Lamp** (C++20 native runtime + embedded V8 + TypeScript SDK).

## Getting Started

1. Start the native V8 server in watch mode:
   \`\`\`bash
   npm run dev
   \`\`\`

2. Add a new module (creates \`<name>.router.ts\` & \`<name>.registry.ts\` and auto-registers into parent router):
   \`\`\`bash
   npx lamp add user
   npx lamp add user/profile
   \`\`\`

3. Build optimized low-level JS bundle and run production server:
   \`\`\`bash
   npm run build
   npm start
   \`\`\`

4. Typecheck your contracts:
   \`\`\`bash
   npm run typecheck
   \`\`\`

## Project Structure

- \`src/app.ts\` — Main application bootstrap (\`createLampApp\`, \`log: true\`)
- \`src/router.ts\` — Root router (\`createLampRouter\`) where all feature modules are mounted
- \`src/registry.ts\` — Root business-logic registry (\`createLampRegistry\`, \`createErrorHandler\`)
- \`tsconfig.json\` — Strict TypeScript configuration for Node >= 22.18 / 24
`;

  fs.writeFileSync(
    path.join(targetDir, "package.json"),
    `${JSON.stringify(packageJson, null, 2)}\n`,
    "utf8"
  );
  fs.writeFileSync(
    path.join(targetDir, "tsconfig.json"),
    `${JSON.stringify(tsConfig, null, 2)}\n`,
    "utf8"
  );
  fs.writeFileSync(path.join(targetDir, ".gitignore"), gitignore, "utf8");
  fs.writeFileSync(path.join(targetDir, "README.md"), readme, "utf8");
  fs.writeFileSync(path.join(targetDir, "src", "app.ts"), appTs, "utf8");
  fs.writeFileSync(path.join(targetDir, "src", "router.ts"), routerTs, "utf8");
  fs.writeFileSync(path.join(targetDir, "src", "registry.ts"), registryTs, "utf8");
}

async function main(): Promise<void> {
  const args = process.argv.slice(2);
  let projectArg: string | undefined;
  let yes = false;
  let install = true;

  for (const arg of args) {
    if (arg === "-h" || arg === "--help") {
      printHelp();
      process.exit(0);
    } else if (arg === "-y" || arg === "--yes") {
      yes = true;
    } else if (arg === "--no-install") {
      install = false;
    } else if (arg.startsWith("-")) {
      console.error(`Unknown option: ${arg}`);
      printHelp();
      process.exit(2);
    } else if (!projectArg) {
      projectArg = arg;
    }
  }

  let projectName = projectArg;
  if (!projectName) {
    if (yes) {
      projectName = "my-lamp-app";
    } else {
      const rl = createInterface({ input: process.stdin, output: process.stdout });
      try {
        const answer = await rl.question("Project name (my-lamp-app): ");
        projectName = answer.trim() || "my-lamp-app";
      } finally {
        rl.close();
      }
    }
  }

  const targetDir = path.resolve(process.cwd(), projectName);
  console.log(`\nScaffolding Lamp project in ${targetDir}...`);

  scaffoldProject({ projectName, targetDir, install });

  if (install) {
    console.log("Installing dependencies with npm...");
    const result =
      process.platform === "win32"
        ? spawnSync("cmd.exe", ["/c", "npm", "install"], {
            cwd: targetDir,
            stdio: "inherit",
          })
        : spawnSync("npm", ["install"], {
            cwd: targetDir,
            stdio: "inherit",
          });
    if (result.status !== 0) {
      console.warn("\nnpm install did not complete cleanly. Run `npm install` inside the project directory.");
    }
  }

  const relDir = path.relative(process.cwd(), targetDir) || ".";
  console.log(`\nDone! Next steps:\n`);
  if (relDir !== ".") {
    console.log(`  cd ${relDir}`);
  }
  if (!install) {
    console.log("  npm install");
  }
  console.log("  npm run dev");
  console.log("  npm run client\n");
}

const currentFile = fs.realpathSync(fileURLToPath(import.meta.url));
const invokedFile = process.argv[1] ? fs.realpathSync(process.argv[1]) : "";
if (invokedFile === currentFile) {
  main().catch((err) => {
    console.error(err instanceof Error ? err.message : String(err));
    process.exit(1);
  });
}
