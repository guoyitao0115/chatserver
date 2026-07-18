FROM node:20-bookworm-slim
WORKDIR /app
COPY package.json gateway.mjs ./
COPY public ./public
EXPOSE 8080
CMD ["node", "gateway.mjs"]
