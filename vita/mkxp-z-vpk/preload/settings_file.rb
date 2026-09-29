# SPDX-License-Identifier: GPL-3.0-or-later
# Loaded by the filesystem binding before game preloads, because the engine's
# settings writer needs it. It wraps nothing: no File, Kernel, IO or Marshal
# method is touched and no game path is ever passed in. A game's saves keep
# upstream semantics (vita/docs/config.md#save-recovery).
#
# The only file handled here is ours and regenerable: settings at userConfPath.
# Each successful publish rotates the previous generation into <path>.bak, so
# recovery restores the last settings the player kept; a write interrupted
# before the final rename leaves the previous file intact.
module VitaSettingsFile
  OPEN = File.method(:open)
  ACTIVE = {}
  LOCK = Mutex.new

  def self.key(path)
    File.realdirpath(path).b
  rescue SystemCallError
    File.expand_path(path).b
  end

  def self.claim(path, reading = false)
    name = key(path)
    LOCK.synchronize do
      if ACTIVE.key?(name)
        return if reading
        raise IOError, 'settings file already open for writing'
      end
      ACTIVE[name] = [name]
    end
  end

  def self.release(lease)
    LOCK.synchronize { ACTIVE.delete(lease[0]) if ACTIVE[lease[0]].equal?(lease) } if lease
  end

  def self.with_file(path, mode)
    file = OPEN.call(path, mode)
    error = nil
    begin
      yield file
    rescue Exception => error
      raise
    ensure
      close_preserving_error(file, error)
    end
  end

  def self.close_preserving_error(file, error = $!)
    begin
      file.close unless file.closed?
    rescue Exception
      raise unless error
    end
  end

  def self.sync(file)
    file.flush
    file.fsync
  end

  def self.copy(from, to)
    with_file(from, 'rb') do |input|
      with_file(to, 'wb') do |output|
        while (chunk = input.read(65536))
          raise IOError, 'short settings copy' unless output.write(chunk) == chunk.bytesize
        end
        sync(output)
      end
    end
  end

  def self.nonempty?(path)
    File.file?(path) && File.size(path) > 0
  end

  def self.retain(path, destination)
    kept = destination
    index = 0
    while File.exist?(kept)
      index += 1
      kept = destination + ".#{index}"
    end
    File.rename(path, kept)
  end

  def self.recover_locked(path)
    return if nonempty?(path)
    return unless nonempty?(path + '.bak')
    copy(path + '.bak', path + '.tmp')
    if File.exist?(path)
      # A previous interrupted attempt is evidence too; never overwrite it.
      retain(path, path + '.corrupt')
    end
    File.rename(path + '.tmp', path)
  end

  # Called once by the filesystem binding at initialization, for our own file.
  def self.recover(path)
    Thread.handle_interrupt(Exception => :never) do
      lease = nil
      begin
        lease = claim(path, true)
        recover_locked(path) if lease
      ensure
        release(lease)
      end
    end
  end

  def self.before_truncate(path)
    # Rotate the last published generation into .bak, so recovery restores the
    # settings the player last kept instead of the first ones ever written. A
    # partial write never reaches the active name and only a nonempty active
    # file is copied, so .bak always holds a complete file; newlib's
    # non-atomic rename can cost the backup itself, never the active file.
    return unless nonempty?(path)
    copy(path, path + '.bak.tmp')
    File.rename(path + '.bak.tmp', path + '.bak')
  end

  # Called by saveUserSettings. Writes a complete temporary, rotates the
  # previous generation to .bak, then renames: the destination is never a
  # partial file.
  def self.write_bytes(path, bytes)
    Thread.handle_interrupt(Exception => :never) do
      lease = nil
      begin
        lease = claim(path)
        recover_locked(path)
        with_file(path + '.tmp', 'wb') do |file|
          raise IOError, 'short settings write' unless file.write(bytes) == bytes.bytesize
          sync(file)
        end
        before_truncate(path)
        File.rename(path + '.tmp', path)
      ensure
        release(lease)
      end
    end
    nil
  end
end
