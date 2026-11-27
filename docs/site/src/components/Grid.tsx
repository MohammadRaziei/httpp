import React from 'react';
import styles from './Grid.module.css';

// A table with a header row and any number of columns: the cells may hold `inline code`, written with backticks. The look is the one of
// the "API at a glance" table (Differ), without the language tabs.
function cell(text: string): React.ReactNode {
  return text.split('`').map((part, i) => (i % 2 ? <code key={i}>{part}</code> : <React.Fragment key={i}>{part}</React.Fragment>));
}

export default function Grid({head, rows}: {head: string[]; rows: string[][]}) {
  return (
    <div className={styles.wrap}>
      <table className={styles.table}>
        <thead>
          <tr>
            {head.map((h) => (
              <th key={h} scope="col">
                {h}
              </th>
            ))}
          </tr>
        </thead>
        <tbody>
          {rows.map((row) => (
            <tr key={row.join('|')}>
              {row.map((text, i) =>
                i === 0 ? (
                  <th key={i} scope="row" className={styles.what}>
                    {cell(text)}
                  </th>
                ) : (
                  <td key={i}>{cell(text)}</td>
                ),
              )}
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
