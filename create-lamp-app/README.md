# create-lamp-app

Project scaffolder for **Lamp** TypeScript backends powered by the native C++20 + embedded V8 runtime.

## Usage

```bash
npx create-lamp-app@latest my-server
# or non-interactive:
npx create-lamp-app@latest my-server --yes
```

## Generated Project Structure

```text
my-server/
├── package.json
├── tsconfig.json
└── src/
    ├── app.ts        # Main application bootstrap (createLampApp, log: true)
    ├── router.ts     # Root router (target: "app")
    └── registry.ts   # Root business-logic registry (ping + createErrorHandler)
```

## Next Steps

```bash
cd my-server
npm run dev           # Start in watch mode (lamp serve src/app.ts --port 3000 --watch)
npx lamp add users    # Scaffold a feature module and auto-register into root router
npm run build         # Bundle into dist/app.js
npm start             # Run production bundle (lamp serve dist/app.js --port 3000)
```

## License

[MIT](LICENSE)
