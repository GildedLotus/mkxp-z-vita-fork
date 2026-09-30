# SPDX-License-Identifier: GPL-3.0-or-later
# Loaded by the filesystem binding before game preloads, because the engine's
# settings writer needs it. It wraps nothing: no File, Kernel, IO or Marshal
# method is touched and no game path is ever passed in. A game's saves keep
# upstream semantics (vita/docs/config.md#save-recovery).
#
# The only file handled here is ours and regenerable: settings at userConfPath.
# Each successful publish rotates the previous valid generation into <path>.bak,
# so recovery restores the last settings the player kept; an invalid active
# file is quarantined as <path>.corrupt (two copies at most), never rotated
# over the backup, bytes that would not read back are refused before they are
# published, and a write interrupted before the final rename leaves the
# previous file intact.
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

  # VitaSettingsFile.valid?(path) is defined by the engine (config.cpp) before
  # recover runs: the one rule for a usable generation -- a nonempty regular
  # file within the config size bound that parses to a JSON object -- shared
  # with the boot-time selection and the CFG[] reader.

  # Two quarantine slots at most (<destination> and <destination>.1): the first
  # is the evidence of the original fault, and a later one replaces the newest
  # copy, so a game that keeps writing bad data cannot fill the card.
  QUARANTINE_SLOTS = 2

  def self.retain(path, destination)
    kept = destination
    (1...QUARANTINE_SLOTS).each do |index|
      break unless File.exist?(kept)
      kept = destination + ".#{index}"
    end
    File.rename(path, kept)
  end

  def self.recover_locked(path)
    return if valid?(path)
    return unless valid?(path + '.bak')
    copy(path + '.bak', path + '.tmp')
    if File.exist?(path)
      # An invalid generation and any earlier interrupted attempt are evidence
      # too; never overwrite them. The backup stays: it is only copied.
      retain(path, path + '.corrupt')
    end
    File.rename(path + '.tmp', path)
  end

  # Called once by the filesystem binding at initialization, for our own file.
  # A failure here must not stop the binding: the boot merge already selected
  # a readable generation, and the next write recovers again.
  def self.recover(path)
    Thread.handle_interrupt(Exception => :never) do
      lease = nil
      begin
        lease = claim(path, true)
        recover_locked(path) if lease
      rescue StandardError => error
        begin
          warn "VitaSettingsFile.recover failed: #{error.class}: #{error.message}"
        rescue StandardError
          nil
        end
      ensure
        release(lease)
      end
    end
    nil
  end

  # The vita_publish commit order: move the last valid generation aside to
  # .bak, then rename the finished temporary onto the free name, so no rename
  # ever targets an occupied active file and a failure leaves a complete
  # generation reachable. An invalid active file is quarantined instead of
  # rotated, so it can never replace a good backup.
  def self.publish(path)
    if File.exist?(path)
      if valid?(path)
        File.rename(path, path + '.bak')
      else
        retain(path, path + '.corrupt')
      end
    end
    File.rename(path + '.tmp', path)
  end

  # Called by saveUserSettings. Writes a complete temporary, then publishes it
  # in the commit order: the destination is never a partial file.
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
        # The same rule the reader applies: bytes that would not be read back
        # are refused here, so the last valid generation stays active instead
        # of being rotated into .bak behind an unreadable file.
        unless valid?(path + '.tmp')
          begin
            File.delete(path + '.tmp')
          rescue SystemCallError
            nil
          end
          raise IOError, 'settings are too large or malformed to be read back'
        end
        publish(path)
      ensure
        release(lease)
      end
    end
    nil
  end
end
