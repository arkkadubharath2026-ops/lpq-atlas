"use client";
// Map + trace overlay.
//
// Three distinct layers, never confused (spec 10.1):
//   * basemap tiles -- OSM-derived imagery for orientation only;
//   * the processed road graph -- segments from the API's viewport endpoint,
//     i.e. the arcs of the graph the engine actually searched;
//   * the execution overlay -- node states replayed from the engine's trace,
//     drawn on a canvas from a projected coordinate cache. Node coordinates
//     live in a typed array held by the parent, never in React state.
import { useEffect, useLayoutEffect, useRef, useState } from "react";
import type * as Leaflet from "leaflet";
import { api } from "@/lib/api";
import { NODE_FRONTIER, NODE_SETTLED, type Replay } from "@/lib/trace";

export interface PointMark {
  lon: number;
  lat: number;
  node: number;
  clicked?: { lon: number; lat: number };
  /** Great-circle metres from the click to the snapped node, from the API. */
  snapM?: number;
}

interface Props {
  datasetId: string | null;
  coords: Float32Array | null;
  bbox?: [number, number, number, number] | null;
  replay: Replay | null;
  replayVersion: number;
  settledColor: string;
  path: number[] | null;
  source: PointMark | null;
  target: PointMark | null;
  onMapClick?: (lon: number, lat: number) => void;
  showRoads?: boolean;
  label?: string;
}

const TILE_SIZE = 256;

function mercator(coords: Float32Array, zoom: number): Float32Array {
  const n = coords.length / 2;
  const out = new Float32Array(coords.length);
  const scale = TILE_SIZE * Math.pow(2, zoom);
  for (let i = 0; i < n; i++) {
    const lon = coords[2 * i];
    const lat = Math.max(-85.05112878, Math.min(85.05112878, coords[2 * i + 1]));
    const s = Math.sin((lat * Math.PI) / 180);
    out[2 * i] = ((lon + 180) / 360) * scale;
    out[2 * i + 1] = (0.5 - Math.log((1 + s) / (1 - s)) / (4 * Math.PI)) * scale;
  }
  return out;
}

export default function MapView(props: Props) {
  const div = useRef<HTMLDivElement>(null);
  const L = useRef<typeof Leaflet | null>(null);
  const map = useRef<Leaflet.Map | null>(null);
  const canvas = useRef<HTMLCanvasElement | null>(null);
  const proj = useRef<{ zoom: number; xy: Float32Array; coords: Float32Array } | null>(null);
  const roads = useRef<Leaflet.Polyline | null>(null);
  const pathLayer = useRef<Leaflet.Polyline | null>(null);
  const marks = useRef<Leaflet.LayerGroup | null>(null);
  const cursor = useRef<Leaflet.CircleMarker | null>(null);
  const tiles = useRef<Leaflet.TileLayer | null>(null);
  const [showBase, setShowBase] = useState(true);
  const latest = useRef(props);
  useLayoutEffect(() => {
    latest.current = props;
  });
  const drawnToken = useRef<{ replay: Replay | null; token: number }>({ replay: null, token: -1 });
  const [ready, setReady] = useState(false);
  const [roadMsg, setRoadMsg] = useState<string | null>(null);

  // --- drawing -----------------------------------------------------------
  const drawNodes = (nodes: Iterable<number> | null) => {
    const m = map.current;
    const c = canvas.current;
    const { coords, replay, settledColor } = latest.current;
    if (!m || !c || !coords || !replay) {
      if (c) c.getContext("2d")?.clearRect(0, 0, c.width, c.height);
      return;
    }
    const zoom = m.getZoom();
    if (!proj.current || proj.current.zoom !== zoom || proj.current.coords !== coords) {
      proj.current = { zoom, xy: mercator(coords, zoom), coords };
    }
    const xy = proj.current.xy;
    const origin = m.getPixelOrigin();
    const offset = m.containerPointToLayerPoint([0, 0]);
    const ox = origin.x + offset.x;
    const oy = origin.y + offset.y;
    const dpr = window.devicePixelRatio || 1;
    const ctx = c.getContext("2d");
    if (!ctx) return;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    const size = zoom >= 16 ? 4 : zoom >= 14 ? 3 : 2;
    const half = size / 2;
    const st = replay.nodeState;
    const paint = (i: number) => {
      const s = st[i];
      if (s === 0) return;
      ctx.fillStyle = s === NODE_SETTLED ? settledColor : "#7dd3fc";
      ctx.fillRect(xy[2 * i] - ox - half, xy[2 * i + 1] - oy - half, size, size);
    };
    if (nodes === null) {
      ctx.clearRect(0, 0, c.width, c.height);
      for (let i = 0; i < st.length; i++) paint(i);
    } else {
      for (const i of nodes) paint(i);
    }
  };

  const fullRedraw = () => {
    const m = map.current;
    const c = canvas.current;
    if (!m || !c) return;
    const size = m.getSize();
    const dpr = window.devicePixelRatio || 1;
    if (c.width !== size.x * dpr || c.height !== size.y * dpr) {
      c.width = size.x * dpr;
      c.height = size.y * dpr;
      c.style.width = `${size.x}px`;
      c.style.height = `${size.y}px`;
    }
    latest.current.replay?.drainDirty();
    drawNodes(null);
  };

  // --- road graph from the viewport API ----------------------------------
  const roadTimer = useRef<number | null>(null);
  const loadRoads = () => {
    if (roadTimer.current) window.clearTimeout(roadTimer.current);
    roadTimer.current = window.setTimeout(async () => {
      const m = map.current;
      const Lf = L.current;
      const { datasetId, coords, showRoads } = latest.current;
      if (!m || !Lf || !datasetId || !coords || showRoads === false) return;
      const b = m.getBounds();
      try {
        const v = await api.viewport(datasetId, [b.getWest(), b.getSouth(), b.getEast(), b.getNorth()]);
        if (latest.current.datasetId !== datasetId) return;
        roads.current?.remove();
        roads.current = null;
        if (v.too_many) {
          setRoadMsg(`Zoom in to draw the road graph (${v.count.toLocaleString()} segments here)`);
          return;
        }
        setRoadMsg(null);
        const lines = v.segments.map((s) => [
          [s[1], s[0]],
          [s[3], s[2]],
        ]) as Leaflet.LatLngExpression[][];
        roads.current = Lf.polyline(lines, {
          color: "#3b5068",
          weight: 1,
          opacity: 0.9,
          interactive: false,
        }).addTo(m);
        roads.current.bringToBack();
      } catch {
        setRoadMsg("Road graph unavailable for this view");
      }
    }, 200);
  };

  // --- setup ---------------------------------------------------------------
  useEffect(() => {
    let cancelled = false;
    void import("leaflet").then((Lf) => {
      if (cancelled || !div.current) return;
      L.current = Lf;
      const m = Lf.map(div.current, { preferCanvas: true, zoomSnap: 1, worldCopyJump: false });
      // Standard OSM tiles (tile usage policy: attribution, light interactive
      // use), darkened in CSS. Orientation only: routes come from the graph.
      tiles.current = Lf.tileLayer("https://tile.openstreetmap.org/{z}/{x}/{y}.png", {
        attribution:
          '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors — basemap for orientation only; routes come from the processed graph',
        maxZoom: 19,
        className: "osm-tiles",
      }).addTo(m);
      m.setView([43.73, 7.42], 13);
      const c = document.createElement("canvas");
      c.style.position = "absolute";
      c.style.inset = "0";
      c.style.zIndex = "450";
      c.style.pointerEvents = "none";
      c.setAttribute("aria-hidden", "true");
      m.getContainer().appendChild(c);
      canvas.current = c;
      marks.current = Lf.layerGroup().addTo(m);
      m.on("move resize", fullRedraw);
      m.on("zoomstart", () => {
        if (canvas.current) canvas.current.style.visibility = "hidden";
      });
      m.on("zoomend", () => {
        if (canvas.current) canvas.current.style.visibility = "visible";
        fullRedraw();
      });
      m.on("moveend", loadRoads);
      m.on("click", (e: Leaflet.LeafletMouseEvent) =>
        latest.current.onMapClick?.(e.latlng.lng, e.latlng.lat),
      );
      map.current = m;
      setReady(true);
      fullRedraw();
    });
    return () => {
      cancelled = true;
      map.current?.remove();
      map.current = null;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Fit the dataset.
  useEffect(() => {
    const m = map.current;
    if (!ready || !m) return;
    proj.current = null;
    roads.current?.remove();
    roads.current = null;
    if (props.bbox) {
      const [w, s, e, n] = props.bbox;
      m.fitBounds([
        [s, w],
        [n, e],
      ]);
    }
    loadRoads();
    fullRedraw();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [ready, props.datasetId, props.bbox, props.coords]);

  // Replay progress: incremental unless a full redraw is due.
  useEffect(() => {
    if (!ready) return;
    const r = props.replay;
    const seen = drawnToken.current;
    if (!r || seen.replay !== r || seen.token !== r.redrawToken) {
      drawnToken.current = { replay: r, token: r ? r.redrawToken : -1 };
      fullRedraw();
    } else {
      drawNodes(r.drainDirty());
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [ready, props.replay, props.replayVersion]);

  // Current node: one marker that moves, so nothing stale is left on the canvas.
  useEffect(() => {
    const m = map.current;
    const Lf = L.current;
    if (!ready || !m || !Lf) return;
    const last = props.replay?.lastSettled;
    const c = props.coords;
    if (!last || !c) {
      cursor.current?.remove();
      cursor.current = null;
      return;
    }
    const ll: [number, number] = [c[2 * last.node + 1], c[2 * last.node]];
    if (!cursor.current) {
      cursor.current = Lf.circleMarker(ll, { radius: 6, color: "#ffffff", weight: 2, fill: false, interactive: false }).addTo(m);
    } else cursor.current.setLatLng(ll);
  }, [ready, props.replay, props.replayVersion, props.coords]);

  // Basemap toggle.
  useEffect(() => {
    const m = map.current;
    const t = tiles.current;
    if (!ready || !m || !t) return;
    if (showBase && !m.hasLayer(t)) t.addTo(m);
    if (!showBase && m.hasLayer(t)) t.remove();
  }, [ready, showBase]);

  // Frame the chosen query once both ends are known.
  const framed = useRef<string>("");
  useEffect(() => {
    const m = map.current;
    const { source, target } = props;
    if (!ready || !m || !source || !target) return;
    const key = `${source.node}-${target.node}`;
    if (framed.current === key) return;
    framed.current = key;
    m.fitBounds(
      [
        [Math.min(source.lat, target.lat), Math.min(source.lon, target.lon)],
        [Math.max(source.lat, target.lat), Math.max(source.lon, target.lon)],
      ],
      { padding: [90, 90], maxZoom: 17 },
    );
  }, [ready, props.source, props.target]);

  // Final route: drawn from the engine's returned path, never from the replay.
  useEffect(() => {
    const m = map.current;
    const Lf = L.current;
    if (!ready || !m || !Lf) return;
    pathLayer.current?.remove();
    pathLayer.current = null;
    const { path, coords } = props;
    if (path && path.length > 1 && coords) {
      const ll = path.map((i) => [coords[2 * i + 1], coords[2 * i]] as [number, number]);
      pathLayer.current = Lf.polyline(ll, { color: "#2dd4bf", weight: 4, opacity: 0.95 }).addTo(m);
    }
  }, [ready, props.path, props.coords]);

  // Source / target: the click and the graph node it snapped to.
  useEffect(() => {
    const Lf = L.current;
    const g = marks.current;
    if (!ready || !Lf || !g) return;
    g.clearLayers();
    const add = (p: PointMark | null, color: string, name: string) => {
      if (!p) return;
      if (p.clicked) {
        Lf.circleMarker([p.clicked.lat, p.clicked.lon], {
          radius: 4, color, weight: 1, fill: false, dashArray: "2 2",
        }).addTo(g).bindTooltip(`${name}: where you clicked`);
        Lf.polyline([[p.clicked.lat, p.clicked.lon], [p.lat, p.lon]], {
          color, weight: 1, dashArray: "3 3",
        }).addTo(g);
      }
      Lf.circleMarker([p.lat, p.lon], { radius: 7, color: "#0b0f14", weight: 2, fillColor: color, fillOpacity: 1 })
        .addTo(g)
        .bindTooltip(`${name}: graph node ${p.node}`);
    };
    add(props.source, "#2dd4bf", "Source");
    add(props.target, "#f87171", "Destination");
  }, [ready, props.source, props.target]);

  return (
    <div className="mapwrap">
      <div ref={div} style={{ position: "absolute", inset: 0 }} aria-label={props.label ?? "Map"} role="application" />
      <button
        type="button"
        className="small"
        onClick={() => setShowBase((b) => !b)}
        aria-pressed={showBase}
        style={{ position: "absolute", top: 10, right: 10, zIndex: 600 }}
      >
        Basemap {showBase ? "on" : "off"}
      </button>
      {roadMsg ? (
        <div className="map-overlay-msg">
          <span className="badge">{roadMsg}</span>
        </div>
      ) : null}
    </div>
  );
}
