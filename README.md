# bracelet

```bash
npm install
```
Instala as dependências.

```bash
copy .env.example .env
```
Cria o arquivo `.env`.

### Variáveis de ambiente

- `DATABASE_URL`: URL do Postgres (ex.: conexão do Neon com `sslmode=require`).
- `PORT`: fornecida pelo Render/host (localmente pode ser 3000).

Arquivos de exemplo/local:
- `.env.example`: modelo base sem segredos.
- `.env.dev`: use para desenvolvimento local (`cp .env.dev .env` antes de `npm run dev`).
- `.env.prod`: opcional para testar local com credenciais de produção (`cp .env.prod .env` antes de `npm start`).
No Render, defina apenas `DATABASE_URL` no painel; não suba `.env*` para o repositório.

```bash
npm run dev
```
Inicia o servidor em desenvolvimento (hot reload).

```bash
npm run build
```
Compila o TypeScript para `dist/`.

```bash
npm start
```
Inicia o servidor em produção.

```bash
flyctl auth login
```
Login no Fly.io (uma vez).

```bash
flyctl launch --no-deploy
```
Cria o app (primeira vez).

```bash
flyctl volumes create bracelet_data --region gru --size 1
```
Cria volume para o SQLite (primeira vez).

```bash
flyctl deploy
```
Publica a API em https://bracelet-api-miqueias.fly.dev

```bash
flyctl logs -a bracelet-api-miqueias
```
Mostra os logs em tempo real (produção).
