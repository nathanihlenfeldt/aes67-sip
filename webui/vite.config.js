import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// The production build is served directly by the aes67-sip gateway from `webui_dir`,
// so the app must use relative asset paths and talk to the same origin `/api`.
// During development `npm start` proxies `/api` to a locally running gateway.
export default defineConfig({
  base: './',
  plugins: [react()],
  server: {
    port: 5173,
    proxy: {
      '/api': {
        target: 'http://127.0.0.1:8081',
        changeOrigin: true,
      },
    },
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    sourcemap: false,
  },
});
