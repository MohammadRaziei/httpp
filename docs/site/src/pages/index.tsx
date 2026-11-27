import React, {useState} from 'react';
import Layout from '@theme/Layout';
import Link from '@docusaurus/Link';
import {Highlight, themes} from 'prism-react-renderer';
import languages from '@site/languages.json';
import snippets from '@site/src/generated/snippets.json';
import {useLang} from '@site/src/components/LangContext';
import styles from './index.module.css';

type Entry = {code: string; path: string; url: string};
const data = snippets as unknown as Record<string, {files: Record<string, Record<string, Entry>>}>;
const theme = {...themes.vsDark, plain: {...themes.vsDark.plain, backgroundColor: 'transparent', color: '#d7dde4'}};

function Install() {
  const [done, setDone] = useState(false);
  const copy = () => {
    navigator.clipboard?.writeText('pip install httpp').then(() => {
      setDone(true);
      setTimeout(() => setDone(false), 1600);
    });
  };
  return (
    <button type="button" className={styles.install} onClick={copy} aria-label="Copy pip install httpp">
      <span className={styles.prompt}>$</span> pip install httpp
      <span className={styles.copy}>{done ? 'Copied' : 'Copy'}</span>
    </button>
  );
}

function CodeWindow() {
  const {lang, setLang} = useLang();
  const entry = data.get_route?.files.main?.[lang] ?? data.get_route?.files.main?.[languages[0].id];
  const known = languages.find((l) => l.id === lang) ?? languages[0];
  return (
    <div className={styles.window}>
      <div className={styles.bar}>
        <div role="tablist" aria-label="Language" className={styles.tabs}>
          {languages.map((l) => (
            <button key={l.id} type="button" role="tab" aria-selected={lang === l.id} className={styles.tab} onClick={() => setLang(l.id)}>
              {l.label}
            </button>
          ))}
        </div>
        <span className={styles.file}>{entry?.path.split('/').pop()}</span>
      </div>
      <Highlight theme={theme} code={(entry?.code ?? '').trimEnd().replace(/\n{3,}/g, '\n\n')} language={known.prism ?? 'python'}>
        {({tokens, getLineProps, getTokenProps}) => (
          <pre className={styles.code}>
            {tokens.map((line, i) => (
              <div key={i} {...getLineProps({line})}>
                {line.map((token, k) => (
                  <span key={k} {...getTokenProps({token})} />
                ))}
              </div>
            ))}
          </pre>
        )}
      </Highlight>
      <div className={styles.reply}>
        <span className={styles.replyIn}>$ httpp curl 127.0.0.1:8000/hello/world</span>
        <span className={styles.replyOut}>hello world</span>
      </div>
    </div>
  );
}

const facts = [
  {title: 'No system packages', text: 'Every third-party library is built from source into the wheel. No apt, brew or choco before pip install.', to: '/docs/install', cta: 'Install'},
  {title: 'Async that frees threads', text: 'async def handlers share one event loop and give their worker thread back while they await. Idle connections cost no thread.', to: '/docs/server/get-route', cta: 'Write a route'},
  {title: 'A client that reports errors properly', text: 'Typed exceptions, raise_for_status(), sessions, timeouts and downloads that resume after a break.', to: '/docs/client/get-request', cta: 'Call a server'},
  {title: 'One core, three front ends', text: 'Python, C++ and a command-line tool run the same compiled core, so they behave the same and cost the same.', to: '/docs/benchmarks', cta: 'See the numbers'},
];

export default function Home() {
  return (
    <Layout title="HTTP client and server for Python and C++" description="One pip install gives you an HTTP server, an HTTP client and a C++ library. No system packages.">
      <header className={styles.hero}>
        <div className={styles.heroText}>
          <h1>A server and a client in one pip&nbsp;install.</h1>
          <p>httpp is an HTTP library for Python and C++. The server runs inside your program, and nothing has to be installed on the system first.</p>
          <div className={styles.actions}>
            <Link className={styles.primary} to="/docs/install">Get started</Link>
            <Link className={styles.secondary} to="/docs/">Read the docs</Link>
          </div>
          <Install />
        </div>
        <CodeWindow />
      </header>

      <main className={styles.facts}>
        {facts.map((f) => (
          <section key={f.title} className={styles.fact}>
            <h2>{f.title}</h2>
            <p>{f.text}</p>
            <Link to={f.to}>{f.cta}</Link>
          </section>
        ))}
      </main>

      <footer className={styles.close}>
        <p>Alpha, MIT licensed, built in the open.</p>
        <Link to="https://github.com/mohammadraziei/httpp">Star it or open an issue on GitHub</Link>
      </footer>
    </Layout>
  );
}
