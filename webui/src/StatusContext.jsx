//
//  StatusContext.jsx
//
//  A single 500 ms /api/status poll shared by every page.  Pages consume this
//  context instead of each mounting their own interval.
//

import { createContext, useContext } from 'react';
import useStatus from './hooks/useStatus';

const StatusContext = createContext({
  status: null,
  error: null,
  loading: true,
  refresh: () => {},
});

export function StatusProvider({ children }) {
  const value = useStatus();
  return <StatusContext.Provider value={value}>{children}</StatusContext.Provider>;
}

export function useGlobalStatus() {
  return useContext(StatusContext);
}

export default StatusContext;
