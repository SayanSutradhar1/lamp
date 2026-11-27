import { Navigate, Route, Routes } from "react-router-dom";
import { DocsPage } from "./pages/DocsPage";
import { LandingPage } from "./pages/LandingPage";

export function App() {
  return (
    <Routes>
      <Route path="/" element={<LandingPage />} />
      <Route path="/docs" element={<Navigate to="/docs/concept" replace />} />
      <Route path="/docs/:topic" element={<DocsPage />} />
      <Route path="*" element={<Navigate to="/" replace />} />
    </Routes>
  );
}

export default App;
