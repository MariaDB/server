$" = [__FILE__]

class ScriptLoader
  @@loading_paths = {}

  def initialize(path)
    @base_path = path
  end

  def load_once
    if absolute_path?(@base_path)
      loaded = load_once_path(@base_path)
      if loaded.nil?
        raise LoadError, error_message
      else
        loaded
      end
    else
      $LOAD_PATH.each do |load_path|
        unless absolute_path?(load_path)
          load_path = File.expand_path(load_path)
          if File::ALT_SEPARATOR
            load_path = load_path.gsub(File::ALT_SEPARATOR, "/")
          end
        end
        loaded = load_once_path(File.join(load_path, @base_path))
        return loaded unless loaded.nil?
      end
      raise LoadError, error_message
    end
  end

  private
  def error_message
    "cannot load such file -- #{@base_path}"
  end

  def absolute_path?(path)
    path.start_with?("/") or (/\A[a-z]:\\/i === path)
  end

  def load_once_path(path)
    loaded = load_once_absolute_path(path)
    return loaded unless loaded.nil?

    return nil unless File.extname(path).empty?

    load_once_absolute_path("#{path}.rb")
  end

  def load_once_absolute_path(path)
    return false if $".include?(path)
    return false if @@loading_paths.key?(path)

    return nil unless File.file?(path)

    @@loading_paths[path] = true
    load(path)
    $" << path
    @@loading_paths.delete(path)

    true
  end
end

module Kernel
  def require(path)
    loader = ScriptLoader.new(path)
    loader.load_once
  end
end
