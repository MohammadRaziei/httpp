import React from 'react';
import {LangProvider} from '@site/src/components/LangContext';

export default function Root({children}: {children: React.ReactNode}) {
  return <LangProvider>{children}</LangProvider>;
}
