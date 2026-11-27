# lamp-client

High-speed binary **`LMP1/LCB1`** client SDK for the **Lamp** native C++20 backend runtime. Supports persistent multiplexed TCP (`lmp://`) in Node.js and binary WebSocket (`ws://` / `wss://`) in browsers.

## Installation

```bash
npm install lamp-client
```

## Usage

### Node.js (`lmp://` TCP)
```ts
import { createLampClient, isLampClientError } from "lamp-client";

const client = createLampClient({ url: "lmp://127.0.0.1:3000" });

try {
  const manifest = await client.manifest();
  const res = await client.callWithMeta("app.users.get", { id: "u_1" });
  console.log(res.data, res.meta.cacheHit, res.meta.responseBytes);
} catch (err) {
  if (isLampClientError(err)) {
    console.error(err.code, err.identifier, err.message);
  }
} finally {
  await client.close();
}
```

### Browser / Vite / React (`ws://` WebSocket)
```ts
import { createLampClient } from "lamp-client/browser";

const client = createLampClient({ url: "ws://127.0.0.1:3000" });

// Native binary file upload (zero base64 inflation)
await client.call("app.files.upload", { label: "avatar", file: selectedWebFile });
```

## API

- `client.call(operationId, input?, options?)`
- `client.callWithMeta(operationId, input?, options?)`
- `client.manifest()`
- `client.ping()`
- `client.stats()`
- `client.close()`

## License

[MIT](LICENSE)
