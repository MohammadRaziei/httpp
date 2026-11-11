import React, {createContext, useCallback, useContext, useEffect, useState} from 'react';
import languages from '@site/languages.json';

export type LangId = string;
const KEY = 'httpp-docs-lang';
const ids: string[] = languages.map((l) => l.id);

type Ctx = {lang: LangId; setLang: (id: LangId) => void};
const LangCtx = createContext<Ctx>({lang: ids[0], setLang: () => {}});

export function LangProvider({children}: {children: React.ReactNode}) {
  // The first render uses the default so the static HTML and the client agree; the stored choice is applied after mount.
  const [lang, setLangState] = useState<LangId>(ids[0]);

  useEffect(() => {
    try {
      const fromQuery = new URLSearchParams(window.location.search).get('lang');
      const stored = window.localStorage.getItem(KEY);
      const pick = [fromQuery, stored].find((v) => v && ids.includes(v));
      if (pick) setLangState(pick);
    } catch {
      // Storage can be blocked; the default stays.
    }
  }, []);

  const setLang = useCallback((id: LangId) => {
    if (!ids.includes(id)) return;
    setLangState(id);
    try {
      window.localStorage.setItem(KEY, id);
    } catch {
      // Not persisted; the choice still applies for this visit.
    }
  }, []);

  return <LangCtx.Provider value={{lang, setLang}}>{children}</LangCtx.Provider>;
}

export const useLang = () => useContext(LangCtx);
