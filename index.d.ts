/**
 * The symbol that a wrapper uses to give its tag. All addons that use this module share it, as
 * `Symbol.for('bare.gobject.tag')`.
 */
export const tag: unique symbol

/**
 * The symbol that a wrapper uses to give out a handle to its GObject instance. All addons that use this
 * module share it, as `Symbol.for('bare.gobject.handle')`.
 */
export const handle: unique symbol

/** A GObject instance on its way from one addon to another. Only native code can read it. */
export interface Handle {}

/** A wrapper that other addons may adopt. Where it keeps its tag is up to the addon. */
export interface Wrapper {
  readonly [tag]: number
  readonly [handle]: Handle
}

/** The JavaScript object of an addon that exports the functions of `registry.h`. */
export interface Binding {
  /**
   * Return a token that keeps the GObject instance alive until the token is garbage collected. A tag
   * can be claimed more than once, and the instance stays alive until every token is gone.
   */
  claim(tag: number, wrapper: object): object

  /**
   * Return the first wrapper of `tag` that is still alive, or `null` if there is none. A tag
   * the registry does not know is not an error.
   */
  wrapper(tag: number): object | null

  /** Return a handle that another addon can adopt. */
  handle(tag: number): Handle

  /** Return a tag for the GObject instance in `handle`. */
  adopt(handle: Handle): number

  /** Return the number of GObject instances in the registry. Useful for finding leaks in tests. */
  registrySize(): number
}

/**
 * Return the tag of `object` in the registry of `binding`. If `object` belongs to another
 * addon, take its handle and adopt it into the registry of `binding` first.
 * @throws A `TypeError` when `object` has no handle.
 */
export function adopt(binding: Binding, object: Wrapper): number
export function adopt(binding: Binding, object: null | undefined): null
