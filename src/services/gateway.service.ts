export class GatewayNotConfiguredError extends Error {
  constructor() {
    super(
      "ESP32 gateway URL is not configured. Set ESP32_GATEWAY_URL (e.g. http://192.168.0.42) or pass ?gatewayUrl= on the request.",
    );
    this.name = "GatewayNotConfiguredError";
  }
}

export class GatewayUnreachableError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "GatewayUnreachableError";
  }
}

const DEFAULT_TIMEOUT_MS = 120_000;

function normalizeBaseUrl(url: string): string {
  const trimmed = url.trim().replace(/\/$/, "");
  if (!/^https?:\/\//i.test(trimmed)) {
    throw new GatewayUnreachableError(`Invalid gateway URL: ${url}`);
  }
  return trimmed;
}

export function getConfiguredGatewayUrl(): string | null {
  const url = process.env.ESP32_GATEWAY_URL?.trim();
  return url ? normalizeBaseUrl(url) : null;
}

export function resolveGatewayBaseUrl(override?: string): string {
  const candidate = override?.trim() || process.env.ESP32_GATEWAY_URL?.trim();
  if (!candidate) {
    throw new GatewayNotConfiguredError();
  }
  return normalizeBaseUrl(candidate);
}

export type GatewayProxyResult = {
  status: number;
  body: string;
  contentType: string | null;
  json: unknown;
};

function gatewayTimeoutMs(): number {
  const raw = Number(process.env.ESP32_GATEWAY_TIMEOUT_MS);
  return Number.isFinite(raw) && raw > 0 ? raw : DEFAULT_TIMEOUT_MS;
}

export async function proxyToGateway(
  path: string,
  init?: RequestInit,
  baseUrlOverride?: string,
): Promise<GatewayProxyResult> {
  const base = resolveGatewayBaseUrl(baseUrlOverride);
  const url = `${base}${path}`;
  const timeoutMs = gatewayTimeoutMs();

  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);

  try {
    const response = await fetch(url, {
      ...init,
      signal: controller.signal,
      headers: {
        "Content-Type": "application/json",
        Accept: "application/json",
        ...(init?.headers ?? {}),
      },
    });

    const contentType = response.headers.get("content-type");
    const body = await response.text();

    let json: unknown = body;
    if (contentType?.includes("application/json") && body.length > 0) {
      try {
        json = JSON.parse(body) as unknown;
      } catch {
        json = { raw: body };
      }
    }

    return {
      status: response.status,
      body,
      contentType,
      json,
    };
  } catch (err) {
    if (err instanceof Error && err.name === "AbortError") {
      throw new GatewayUnreachableError(
        `ESP32 gateway timed out after ${timeoutMs}ms (${url})`,
      );
    }

    const message = err instanceof Error ? err.message : String(err);
    throw new GatewayUnreachableError(`Failed to reach ESP32 gateway at ${url}: ${message}`);
  } finally {
    clearTimeout(timer);
  }
}

export function sendGatewayProxy(reply: GatewayReply, result: GatewayProxyResult): unknown {
  const type = result.contentType ?? "application/json";
  const payload =
    result.contentType?.includes("application/json") && typeof result.json === "object"
      ? result.json
      : result.body;

  return reply.status(result.status).type(type).send(payload);
}

/** Minimal reply surface for proxy passthrough (avoids Fastify generic status codes). */
export type GatewayReply = {
  status: (code: number) => GatewayReply;
  type: (contentType: string) => GatewayReply;
  send: (payload: unknown) => unknown;
};
