import { Monitor, Moon, Sun } from "lucide-react";
import { useTheme, type ThemeMode } from "../theme/ThemeContext";

const OPTIONS: readonly { value: ThemeMode; label: string; Icon: typeof Sun }[] = [
  { value: "system", label: "System", Icon: Monitor },
  { value: "light", label: "Light", Icon: Sun },
  { value: "dark", label: "Dark", Icon: Moon },
];

export function ThemeSwitcher() {
  const { mode, setMode } = useTheme();

  return (
    <div className="theme-switcher" role="group" aria-label="Theme mode">
      {OPTIONS.map(({ value, label, Icon }) => {
        const active = mode === value;
        return (
          <button
            key={value}
            type="button"
            className={`theme-btn ${active ? "active" : ""}`}
            onClick={() => setMode(value)}
            title={`Theme: ${label}`}
            aria-pressed={active}
          >
            <Icon size={14} />
            <span>{label}</span>
          </button>
        );
      })}
    </div>
  );
}
