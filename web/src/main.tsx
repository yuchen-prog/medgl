import { createRoot } from 'react-dom/client';
import { App } from './App';
import '@fontsource-variable/ibm-plex-sans/wght.css';
import '@fontsource/ibm-plex-mono/400.css';
import '@fontsource/ibm-plex-mono/500.css';
import './styles.css';

const root = document.getElementById('root');
if (!root) throw new Error('Application root not found');
createRoot(root).render(<App />);