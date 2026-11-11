import React from 'react';
import languages from '@site/languages.json';
import {useLang} from './LangContext';
import styles from './LangSwitch.module.css';

// Navbar control: one choice for every page of the site.
export default function LangSwitch() {
  const {lang, setLang} = useLang();
  return (
    <div className={styles.group} role="radiogroup" aria-label="Language of the code examples">
      {languages.map((l) => (
        <button
          key={l.id}
          type="button"
          role="radio"
          aria-checked={lang === l.id}
          className={styles.option}
          onClick={() => setLang(l.id)}>
          {l.label}
        </button>
      ))}
    </div>
  );
}
