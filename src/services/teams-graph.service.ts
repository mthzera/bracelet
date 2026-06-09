type GraphToken = {
  accessToken: string;
  expiresAt: number;
};

let cachedToken: GraphToken | null = null;

async function getGraphToken(config: {
  tenantId: string;
  clientId: string;
  clientSecret: string;
}): Promise<string> {
  if (cachedToken && Date.now() < cachedToken.expiresAt - 60_000) {
    return cachedToken.accessToken;
  }

  const body = new URLSearchParams({
    client_id: config.clientId,
    client_secret: config.clientSecret,
    scope: "https://graph.microsoft.com/.default",
    grant_type: "client_credentials",
  });

  const res = await fetch(
    `https://login.microsoftonline.com/${config.tenantId}/oauth2/v2.0/token`,
    {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body,
    },
  );

  if (!res.ok) {
    const text = await res.text();
    throw new Error(`Graph token failed (${res.status}): ${text}`);
  }

  const data = (await res.json()) as { access_token: string; expires_in: number };
  cachedToken = {
    accessToken: data.access_token,
    expiresAt: Date.now() + data.expires_in * 1000,
  };
  return data.access_token;
}

async function graphGet<T>(token: string, path: string): Promise<T> {
  const res = await fetch(`https://graph.microsoft.com/v1.0${path}`, {
    headers: { Authorization: `Bearer ${token}` },
  });
  if (!res.ok) {
    const text = await res.text();
    throw new Error(`Graph GET ${path} failed (${res.status}): ${text}`);
  }
  return res.json() as Promise<T>;
}

async function graphPost<T>(token: string, path: string, body: unknown): Promise<T> {
  const res = await fetch(`https://graph.microsoft.com/v1.0${path}`, {
    method: "POST",
    headers: {
      Authorization: `Bearer ${token}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify(body),
  });
  if (!res.ok) {
    const text = await res.text();
    throw new Error(`Graph POST ${path} failed (${res.status}): ${text}`);
  }
  return res.json() as Promise<T>;
}

async function resolveUserId(token: string, email: string): Promise<string> {
  const encoded = encodeURIComponent(email);
  const user = await graphGet<{ id: string }>(token, `/users/${encoded}`);
  return user.id;
}

async function findOrCreateOneOnOneChat(
  token: string,
  senderId: string,
  recipientId: string,
): Promise<string> {
  const filter = encodeURIComponent(
    "chatType eq 'oneOnOne'",
  );
  const chats = await graphGet<{ value: Array<{ id: string; members?: Array<{ userId?: string }> }> }>(
    token,
    `/users/${senderId}/chats?$filter=${filter}&$expand=members`,
  );

  for (const chat of chats.value ?? []) {
    const memberIds = (chat.members ?? []).map((m) => m.userId).filter(Boolean);
    if (memberIds.includes(senderId) && memberIds.includes(recipientId)) {
      return chat.id;
    }
  }

  const created = await graphPost<{ id: string }>(token, "/chats", {
    chatType: "oneOnOne",
    members: [
      {
        "@odata.type": "#microsoft.graph.aadUserConversationMember",
        roles: ["owner"],
        "user@odata.bind": `https://graph.microsoft.com/v1.0/users('${senderId}')`,
      },
      {
        "@odata.type": "#microsoft.graph.aadUserConversationMember",
        roles: ["owner"],
        "user@odata.bind": `https://graph.microsoft.com/v1.0/users('${recipientId}')`,
      },
    ],
  });

  return created.id;
}

export type TeamsReportPayload = {
  recipientEmail: string;
  patientName: string;
  braceletLabel: string;
  windowMinutes: number;
  chartImageUrl: string | null;
  summaryText: string;
  overallStatus: string | null;
};

function buildAdaptiveCard(payload: TeamsReportPayload): object {
  const body: object[] = [
    {
      type: "TextBlock",
      text: `Relatório pulseira — ${payload.patientName}`,
      weight: "Bolder",
      size: "Large",
    },
    {
      type: "TextBlock",
      text: `${payload.braceletLabel} · últimos ${payload.windowMinutes} min`,
      isSubtle: true,
      spacing: "None",
    },
    {
      type: "TextBlock",
      text: payload.summaryText,
      wrap: true,
    },
  ];

  if (payload.chartImageUrl) {
    body.push({
      type: "Image",
      url: payload.chartImageUrl,
      altText: "Gráfico de variação dos sinais vitais",
    });
  }

  if (payload.overallStatus) {
    body.push({
      type: "TextBlock",
      text: `Status clínico: **${payload.overallStatus}**`,
      wrap: true,
    });
  }

  return {
    type: "message",
    attachments: [
      {
        contentType: "application/vnd.microsoft.card.adaptive",
        content: {
          $schema: "http://adaptivecards.io/schemas/adaptive-card.json",
          type: "AdaptiveCard",
          version: "1.4",
          body,
        },
      },
    ],
  };
}

export async function sendTeamsReportViaGraph(
  config: {
    tenantId: string;
    clientId: string;
    clientSecret: string;
    senderEmail: string;
  },
  payload: TeamsReportPayload,
): Promise<void> {
  const token = await getGraphToken(config);
  const senderId = await resolveUserId(token, config.senderEmail);
  const recipientId = await resolveUserId(token, payload.recipientEmail);
  const chatId = await findOrCreateOneOnOneChat(token, senderId, recipientId);

  await graphPost(token, `/chats/${chatId}/messages`, buildAdaptiveCard(payload));
}

export async function sendTeamsReportViaFlowWebhook(
  webhookUrl: string,
  payload: TeamsReportPayload,
): Promise<void> {
  const res = await fetch(webhookUrl, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      recipientEmail: payload.recipientEmail,
      patientName: payload.patientName,
      braceletLabel: payload.braceletLabel,
      windowMinutes: payload.windowMinutes,
      chartImageUrl: payload.chartImageUrl,
      summaryText: payload.summaryText,
      overallStatus: payload.overallStatus,
      adaptiveCard: buildAdaptiveCard(payload),
    }),
  });

  if (!res.ok) {
    const text = await res.text();
    throw new Error(`Teams flow webhook failed (${res.status}): ${text}`);
  }
}
