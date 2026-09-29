# SPDX-License-Identifier: GPL-3.0-or-later
# Loaded by the mandatory win32_wrap entry point, after settings_file and classic.
module VitaPreloads
  DEVICE = %w[settings_file.rb ruby_classic_wrap.rb win32_wrap.rb game_preloads.rb].freeze

  def self.path(entry, base)
    unless entry.is_a?(String) && !entry.empty? && !entry.match?(/[\x00-\x1f\x7f]/)
      raise ArgumentError, 'preloadScript entries must be nonempty path strings'
    end
    entry = entry.tr('\\', '/')
    if entry.match?(/\A[A-Za-z][A-Za-z0-9]*:\//)
      root, tail = entry.split(':/', 2)
      parts = []
      tail.split('/').each do |part|
        next if part.empty? || part == '.'
        part == '..' ? parts.pop : parts.push(part)
      end
      "#{root}:/#{parts.join('/')}"
    elsif entry.include?(':') || entry.start_with?('/')
      raise ArgumentError, 'preloads need a game-relative or device:/ path'
    else
      path("#{base}/#{entry}", base)
    end
  end

  def self.list(value, source)
    raise ArgumentError, "#{source}: preloadScript must be an array" unless value.is_a?(Array)
    value
  end

  def self.run
    return if @running || @done
    compose
  end

  def self.compose
    @running = true
    base = Dir.pwd.tr('\\', '/')
    scripts = []
    %w[mkxp.json mkxp-vita.json].each do |name|
      next unless File.file?(name)
      bytes = File.binread(name).force_encoding('UTF-8').delete_prefix("\uFEFF")
      config = HTTPLite::JSON.parse(bytes)
      raise ArgumentError, "#{name}: expected a config object" unless config.is_a?(Hash)
      scripts = list(config['preloadScript'], name) if config.key?('preloadScript')
    end
    scripts += list(CFG.to_hash.fetch('vitaPackagePreloads', []), 'package')
    seen = DEVICE.map { |name| "app0:/preload/#{name}" }
    # Resolve and validate the whole list before executing any game preload.
    scripts = scripts.map { |entry| path(entry, base) }.reject do |entry|
      duplicate = seen.include?(entry)
      seen << entry unless duplicate
      duplicate
    end
    scripts.each do |entry|
      System.puts("[preload] #{entry}")
      load entry
    end
    @done = true
  ensure
    @running = false
  end
end
