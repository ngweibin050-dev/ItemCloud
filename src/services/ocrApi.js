// src/services/ocrApi.js
import { auth } from './firebase';

const API_BASE = (import.meta.env.VITE_API_BASE_URL || '').replace(/\/+$/, '');
const TIMEOUT_MS = 45000;

/**
 * Ask the backend to OCR the uploaded matric card and compare it with the form.
 * Never throws: on any failure it returns { verdict: 'unavailable' } so sign-up can continue.
 * @returns {Promise<{verdict: 'match'|'partial'|'mismatch'|'unreadable'|'unavailable', matricNoMatch?: boolean, nameScore?: number}>}
 */
export async function checkMatricCard({ imageUrl, matricNo, fullName }) {
  const user = auth.currentUser;
  if (!user || !API_BASE) return { verdict: 'unavailable' };

  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), TIMEOUT_MS);
  try {
    const token = await user.getIdToken();
    const res = await fetch(`${API_BASE}/api/ocr-card`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', Authorization: `Bearer ${token}` },
      body: JSON.stringify({ imageUrl, matricNo, fullName }),
      signal: controller.signal,
    });
    if (!res.ok) return { verdict: 'unavailable' };
    const json = await res.json();
    return {
      verdict: json.verdict,
      matricNoMatch: !!json.matricNoMatch,
      nameScore: Number(json.nameScore) || 0,
    };
  } catch (e) {
    console.warn('[ocrApi] OCR check skipped:', e?.name === 'AbortError' ? 'timeout' : e?.message);
    return { verdict: 'unavailable' };
  } finally {
    clearTimeout(timer);
  }
}
