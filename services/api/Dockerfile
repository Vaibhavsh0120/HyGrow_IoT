FROM node:24-bookworm-slim AS dependencies
WORKDIR /app
COPY services/api/package*.json ./
RUN npm ci --ignore-scripts
COPY services/api/tsconfig.json ./
COPY services/api/src ./src
RUN npm run build && npm prune --omit=dev --ignore-scripts && npm cache clean --force

FROM node:24-bookworm-slim
WORKDIR /app
ENV NODE_ENV=production PORT=8080
ARG SOURCE_REVISION=development
ENV SOURCE_REVISION=$SOURCE_REVISION
COPY --from=dependencies --chown=node:node /app/node_modules ./node_modules
COPY --chown=node:node services/api/package.json ./
COPY --from=dependencies --chown=node:node /app/dist ./dist
COPY --chown=node:node services/api/migrations ./migrations
USER node
EXPOSE 8080
HEALTHCHECK --interval=20s --timeout=5s --start-period=15s CMD node -e "fetch('http://127.0.0.1:8080/health/ready').then(r=>{if(!r.ok)process.exit(1)}).catch(()=>process.exit(1))"
CMD ["node", "dist/server.js"]
