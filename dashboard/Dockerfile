FROM node:22-alpine

WORKDIR /app

# No dependencies — server.js is stdlib only, so there is no npm install step.
COPY package.json server.js ./
COPY public ./public

USER node
EXPOSE 8080

CMD ["node", "server.js"]
