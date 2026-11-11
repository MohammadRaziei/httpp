import React from 'react';
import CodeBlock from '@theme/CodeBlock';
import languages from '@site/languages.json';
import snippets from '@site/src/generated/snippets.json';
import {useLang} from './LangContext';
import styles from './Lang.module.css';

type Entry = {code: string; path: string};
const data = snippets as Record<string, Record<string, Entry>>;

// The code of one snippet in the language chosen in the navbar. The code comes from snippets/<lang>/<name>.<ext>.
export default function Lang({name}: {name: string}) {
  const {lang, setLang} = useLang();
  const entry = data[name]?.[lang];
  const known = languages.find((l) => l.id === lang);

  if (!data[name]) {
    return <p className={styles.missing}>Snippet "{name}" was not found. Run scripts/prepare.py.</p>;
  }

  return (
    <div className={styles.box}>
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
      {entry ? (
        <CodeBlock language={known?.prism ?? 'text'} title={entry.path}>
          {entry.code}
        </CodeBlock>
      ) : (
        <p className={styles.missing}>No {known?.label} version of "{name}" yet.</p>
      )}
    </div>
  );
}
