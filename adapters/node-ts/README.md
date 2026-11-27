# @lamp/node-adapter

Node.js `LMP1/LCB1` binary protocol adapter for running `LampApp` contracts inside a Node.js process when native Node ecosystem packages (such as `mongoose` or `pg`) are required.

## Usage

```ts
import { serveLampNodeAdapter } from "@lamp/node-adapter";
import { createLampApp } from "lamp-app";

const app = createLampApp();
// register operations or routers...

const server = await serveLampNodeAdapter(app, {
  host: "127.0.0.1",
  port: 3000,
  maxCacheTtlMs: 30000,
});
```
