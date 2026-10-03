// TONE3000 logo assets from the API design requirements. Show the full logo
// before the T3K mark so users have context for the short form.

export const Tone3000Logo = ({ className = 'header-logo' }: { className?: string }) => (
  <img src="/brand/tone3000.svg" alt="TONE3000" className={className} />
);

export const T3kMark = ({ className = 'mark' }: { className?: string }) => (
  <img src="/brand/t3k.svg" alt="T3K" className={className} />
);
