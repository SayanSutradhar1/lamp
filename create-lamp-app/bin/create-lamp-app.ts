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
  fs.mkdirSync(path.join(targetDir, "scripts"), { recursive: true });

  const packageJson = {
    name: pkgName,
    version: "0.1.0",
    private: true,
    type: "module",
    scripts: {
      dev: "lamp serve src/app.ts --port 3000",
      start: "lamp serve src/app.ts --port 3000",
      client: "node scripts/client.ts",
      typecheck: "tsc --noEmit",
    },
    dependencies: {
      "lamp-app": "^0.2.0",
      "lamp-client": "^0.2.0",
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
    include: ["src/**/*.ts", "scripts/**/*.ts"],
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

  const appTs = `import { LampError, createLampApp, s } from "lamp-app";

interface UserRecord {
  readonly id: string;
  readonly name: string;
  readonly role: "member" | "admin";
}

const users = new Map<string, UserRecord>([
  ["1", { id: "1", name: "Ada Lovelace", role: "admin" }],
]);

const app = createLampApp({
  transport: "lmp1-binary",
  serialization: "lcb1",
  execution: "parallel-dynamic",
  cache: {
    enabled: true,
    defaultTtl: 60,
    maxTtlLimitSeconds: 300,
  },
});

app.register("health.ping", {
  description: "Health check operation",
  input: s.object({}),
  output: s.object({ ok: s.boolean(), runtime: s.string() }),
  handler: async () => ({
    ok: true,
    runtime: "lamp-v8",
  }),
});

app.register("users.get", {
  description: "Fetch a user by ID with binary wire caching",
  input: s.object({ id: s.string({ minLength: 1 }) }),
  output: s.object({
    id: s.string(),
    name: s.string(),
    role: s.enum(["member", "admin"]),
  }),
  cache: { ttl: 60 },
  handler: async ({ input }) => {
    const user = users.get(input.id);
    if (!user) {
      throw new LampError(\`User "\${input.id}" not found\`, {
        code: "NOT_FOUND",
        identifier: "USER_NOT_FOUND",
      });
    }
    return user;
  },
});

app.register("analytics.parallelSquares", {
  description: "Splits workloads across C++ dynamic worker threads",
  input: s.object({ values: s.array(s.number()) }),
  output: s.object({ squares: s.array(s.number()) }),
  handler: async ({ input, ctx }) => {
    const squares = await ctx.parallel(input.values, (n) => n * n);
    return { squares };
  },
});

await app.start();
`;

  const clientTs = `import { createLampClient } from "lamp-client";

const client = createLampClient({ url: "lmp://127.0.0.1:3000" });

try {
  const health = await client.call("health.ping", {});
  console.log("health.ping =>", health);

  const user = await client.call("users.get", { id: "1" });
  console.log("users.get =>", user);

  const squares = await client.call("analytics.parallelSquares", {
    values: [2, 4, 6, 8],
  });
  console.log("analytics.parallelSquares =>", squares);
} finally {
  await client.close();
}
`;

  const readme = `# ${pkgName}

Backend project built with **Lamp** (C++20 native runtime + embedded V8 + TypeScript SDK).

## Getting Started

1. Start the native V8 server:
   \`\`\`bash
   npm run dev
   \`\`\`

2. In another terminal, call operations via \`lamp-client\`:
   \`\`\`bash
   npm run client
   \`\`\`

3. Typecheck your contracts:
   \`\`\`bash
   npm run typecheck
   \`\`\`

## Project Structure

- \`src/app.ts\` — Typed operation contracts (\`createLampApp\`, \`s\`) executed by \`lamp_server\`
- \`scripts/client.ts\` — Binary \`LMP1/LCB1\` client example (\`createLampClient\`)
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
  fs.writeFileSync(path.join(targetDir, "scripts", "client.ts"), clientTs, "utf8");
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
