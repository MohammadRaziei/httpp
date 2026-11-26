import React from 'react';
import languages from '@site/languages.json';
import {useLang} from './LangContext';
import styles from './Differ.module.css';

// [what, python, cpp]: the cells may hold `inline code`, written with backticks.
export type Row = [string, string, string];

// A cell as text and <code> pieces.
function cell(text: string): React.ReactNode {
  return text.split('`').map((part, i) => (i % 2 ? <code key={i}>{part}</code> : <React.Fragment key={i}>{part}</React.Fragment>));
}

// The "API at a glance" table of a page: it shows the language chosen in the navbar, and has the same tabs as the code boxes, so it
// follows the switch like the code does and can be switched from here.
export default function Differ({rows}: {rows: Row[]}) {
  const {lang, setLang} = useLang();
  const column = Math.max(0, languages.findIndex((l) => l.id === lang)) + 1;
  return (
    <div className={styles.wrap}>
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
      <table className={styles.table}>
        <tbody>
          {rows.map((row) => (
            <tr key={row[0]}>
              <th scope="row" className={styles.what}>
                {row[0]}
              </th>
              <td>{cell(row[column])}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
