import type { EmbeddedUser } from '../types';
import { Tone3000Logo, T3kMark } from './Brand';
import { CrossOriginImage } from './CrossOriginImage';

interface Props {
  user: EmbeddedUser | null;
  onBrowse: (() => void) | null;
}

export function Header({ user, onBrowse }: Props) {
  return (
    <header className="header">
      <div className="header-brand">
        <Tone3000Logo />
        <span className="header-sub">DIY PEDAL</span>
      </div>
      <div className="header-right">
        {user && (
          <>
            {user.avatar_url && <CrossOriginImage src={user.avatar_url} alt="" className="avatar" />}
            <span className="muted">@{user.username}</span>
          </>
        )}
        {onBrowse && (
          <button className="btn" onClick={onBrowse}>
            <T3kMark />
            Browse
          </button>
        )}
      </div>
    </header>
  );
}
