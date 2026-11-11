import React from 'react';
import {useLang} from './LangContext';

// Text that applies to one language only. Use it where the two APIs really differ.
export default function Only({lang, children}: {lang: string; children: React.ReactNode}) {
  const current = useLang().lang;
  return current === lang ? <>{children}</> : null;
}
