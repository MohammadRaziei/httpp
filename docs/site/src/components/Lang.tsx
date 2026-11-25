import React from 'react';
import CodeBlock from '@theme/CodeBlock';
import languages from '@site/languages.json';
import snippets from '@site/src/generated/snippets.json';
import {useLang} from './LangContext';
import styles from './Lang.module.css';

type Entry = {code: string; path: string; url: string};
// per topic: `readme` (the topic folder on GitHub) and `files`: one Entry per language id for each file stem
// (`main` is the example of the page, any other stem is an extra shown below it)
type Topic = {readme: string; files: Record<string, Record<string, Entry>>};
const data = snippets as unknown as Record<string, Topic>;

// The code of one example in the language chosen in the navbar. The code comes from examples/<name>/<lang>/<file>.<ext>
// (`file` defaults to `main`); the file name above it links to that file on GitHub.
export default function Lang({name, file = 'main'}: {name: string; file?: string}) {
  const {lang, setLang} = useLang();
  const entry = data[name]?.files?.[file]?.[lang];
  const known = languages.find((l) => l.id === lang);

  if (!data[name]?.files?.[file]) {
    return (
      <p className={styles.missing}>
        Snippet "{name}/{file}" was not found. Run scripts/prepare.py.
      </p>
    );
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
