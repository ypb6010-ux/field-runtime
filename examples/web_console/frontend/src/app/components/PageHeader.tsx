import type { ReactNode } from "react";

interface PageHeaderProps {
  title: string;
  en: string;
  description?: string;
  actions?: ReactNode;
}

export function PageHeader({ title, en, description, actions }: PageHeaderProps) {
  return (
    <div className="flex items-start justify-between gap-4 border-b border-border bg-card px-6 py-4">
      <div>
        <div className="flex items-baseline gap-2">
          <h1>{title}</h1>
          <span className="text-sm text-muted-foreground">{en}</span>
        </div>
        {description && <p className="mt-0.5 text-sm text-muted-foreground">{description}</p>}
      </div>
      {actions && <div className="flex shrink-0 items-center gap-2">{actions}</div>}
    </div>
  );
}
