import { defineConfig, UserConfig } from 'vite'
import { svelte } from '@sveltejs/vite-plugin-svelte'
import path from 'path'
import { fileURLToPath } from 'url'
import sveltePreprocess from 'svelte-preprocess'

// __dirname shim for ESM
const __filename = fileURLToPath(import.meta.url)
const __dirname = path.dirname(__filename)

// https://vitejs.dev/config/
//
// NOTE on the @svelte-use/* alias: the upstream author had a sibling
// `svelte-use` repo checked out and wanted dev mode to load the local
// .mjs files instead of node_modules. We resolve from node_modules
// unconditionally so a fresh clone works without the sibling repo.
export default defineConfig(() => {
  const config: UserConfig = {
    root: 'client',
    build: {
      outDir: path.join(__dirname, 'dist'),
      emptyOutDir: true,
    },
    resolve: {
      alias: {},
    },
    plugins: [
      svelte({
        preprocess: sveltePreprocess(),
      }),
    ],
  }
  return config
})
