import React from 'react';
import CodeBlock from '@theme/CodeBlock';
import languages from '@site/languages.json';
import snippets from '@site/src/generated/snippets.json';
import {useLang} from './LangContext';
import styles from './Lang.module.css';

type Entry = {code: string; path: string; url: string};
// per topic: one Entry per language id, plus `readme` (the topic folder on GitHub)
const data = snippets as unknown as Record<string, Record<string, Entry | string>>;

// The code of one example in the language chosen in the navbar. The code comes from examples/<name>/<lang>/main.<ext>;
// the file name above it links to that file on GitHub.
export default function Lang({name}: {name: string}) {
  const {lang, setLang} = useLang();
  const entry = data[name]?.[lang] as Entry | undefined;
  const known = languages.find((l) => l.id === lang);

  if (!data[name]) {
    return <p className={styles.missing}>Snippet "{name}" was not found. Run scripts/prepare.py.</p>;
  }

  return (
    <div className={styles.box}>
      <div className={styles.bar}>
        <div className={styles.tabs} role="tablist" aria-label="Language">
          {languages.map((l) => (
            <button
              key={l.id}
              type="button"
              role="tab"
              aria-selected={lang === l.id}
              className={styles.tab}
              onClick={() => setLang(l.id)}>
              {l.label}
            </button>
          ))}
        </div>
        {entry && (
          <a className={styles.file} href={entry.url} target="_blank" rel="noopener noreferrer">
            {entry.path.replace(/^examples\//, '')}
          </a>
        )}
      </div>
      {entry ? (
        <CodeBlock language={known?.prism ?? 'text'}>{entry.code}</CodeBlock>
      ) : (
        <p className={styles.missing}>No {known?.label} version of "{name}" yet.</p>
      )}
    </div>
  );
}
