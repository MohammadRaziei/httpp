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

// The "Where the two differ" table of a page: it shows the language chosen in the navbar, so it follows the switch like the code does.
export default function Differ({rows}: {rows: Row[]}) {
  const {lang} = useLang();
  const column = Math.max(0, languages.findIndex((l) => l.id === lang)) + 1;
  const label = languages.find((l) => l.id === lang)?.label ?? '';
  return (
    <div className={styles.wrap}>
      <table className={styles.table}>
        <thead>
          <tr>
            <th scope="col" className={styles.what}>
              What
            </th>
            <th scope="col">{label}</th>
          </tr>
        </thead>
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
